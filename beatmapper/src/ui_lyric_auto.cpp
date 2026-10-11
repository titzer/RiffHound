#include "ui_lyric_auto.h"
#include "lyric_align.h"
#include "stems.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>

static bool  s_active = false;
static int   s_hover  = -1;
static char  s_status[160] = "";
static std::vector<LyricSuggestion> s_sugg;
static std::vector<double>          s_dismissed;   // keys left out of the alignment

// Envelope of the vocal stem, kept until the stems change.
static LyricActivity s_act;
static const float*  s_act_pcm = nullptr;
static int           s_act_gen = -1;

static uint64_t s_sig = 0;   // inputs the suggestions were computed from
static int      s_sep_requested = -1;   // load_count we started a separation for

// Cursor, by identity (suggestions are rebuilt whenever the lyrics change).
static bool   s_cur_set = false;
static double s_cur_key = 0.0;
static char   s_cur_text[128] = "";
static bool   s_cur_moved = false;
static double s_cur_after = -1.0;   // >= 0: put the cursor on the first suggestion from here

static void cursor_to(int i) {
    if (i < 0 || i >= (int)s_sugg.size()) { s_cur_set = false; return; }
    s_cur_set = true;
    s_cur_key = s_sugg[i].key;
    strncpy(s_cur_text, s_sugg[i].text, sizeof(s_cur_text) - 1);
    s_cur_text[sizeof(s_cur_text) - 1] = 0;
    s_cur_moved = true;
}

int ui_lyric_auto_cursor() {
    if (!s_cur_set) return -1;
    for (int i = 0; i < (int)s_sugg.size(); i++)
        if (s_sugg[i].key == s_cur_key && strcmp(s_sugg[i].text, s_cur_text) == 0) return i;
    return -1;
}

void ui_lyric_auto_advance() {
    if (s_sugg.empty()) return;
    int c = ui_lyric_auto_cursor();
    if (c < 0) { cursor_to(0); return; }
    // Suggestions are in lyric order, which with lines placed in the middle
    // is not quite time order: step to the next start after this one.
    int best = -1;
    for (int i = 0; i < (int)s_sugg.size(); i++)
        if (s_sugg[i].t0 > s_sugg[c].t0 + 1e-6 &&
                (best < 0 || s_sugg[i].t0 < s_sugg[best].t0)) best = i;
    if (best < 0)                                  // wrap to the earliest
        for (int i = 0; i < (int)s_sugg.size(); i++)
            if (best < 0 || s_sugg[i].t0 < s_sugg[best].t0) best = i;
    cursor_to(best);
}

void ui_lyric_auto_set_cursor(int i) { cursor_to(i); }

bool ui_lyric_auto_take_cursor_moved() {
    bool m = s_cur_moved;
    s_cur_moved = false;
    return m && ui_lyric_auto_cursor() >= 0;
}

bool ui_lyric_auto_wants_l(const AudioState* audio) {
    return s_active && !s_sugg.empty() && !audio->playing;
}

bool ui_lyric_auto_active() { return s_active; }

void ui_lyric_auto_set_active(bool on) {
    if (on == s_active) return;
    s_active = on;
    s_cur_set = false;
    s_cur_after = -1.0;
    s_sugg.clear();
    s_dismissed.clear();
    s_sig = 0;
    s_hover = -1;
}

void ui_lyric_auto_reset() {
    s_cur_set = false;
    s_cur_after = -1.0;
    s_sugg.clear();
    s_dismissed.clear();
    s_sig = 0;
    s_hover = -1;
    s_act = LyricActivity();
    s_act_pcm = nullptr;
    s_act_gen = -1;
}

const char* ui_lyric_auto_status() { return s_status; }
int  ui_lyric_auto_count()          { return (int)s_sugg.size(); }
const LyricSuggestion* ui_lyric_auto_get(int i) {
    return (i >= 0 && i < (int)s_sugg.size()) ? &s_sugg[i] : nullptr;
}
int  ui_lyric_auto_hover()          { return s_hover; }
void ui_lyric_auto_set_hover(int i) { s_hover = i; }

void ui_lyric_auto_dismiss(double key) {
    s_dismissed.push_back(key);
    s_sig = 0;                       // recompute without it
}

static bool is_dismissed(double key) {
    for (double k : s_dismissed) if (k == key) return true;
    return false;
}

static void hash_bytes(uint64_t* h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) { *h ^= b[i]; *h *= 1099511628211ull; }
}

static int vocals_index() {
    for (int i = 0; i < stems_count(); i++)
        if (strcmp(stems_name(i), "vocals") == 0) return i;
    return -1;
}

static bool is_unplaced(const Lyric& ly, double dur) { return ly.t_start >= dur - 1e-9; }

void ui_lyric_auto_update(const AudioState* audio, const LyricMap* lm) {
    if (!s_active) return;
    if (!audio->loaded || audio->duration <= 0.0) {
        snprintf(s_status, sizeof(s_status), "No track loaded.");
        s_sugg.clear(); s_sig = 0;
        return;
    }
    StemsStatus st = stems_status();
    int vi = vocals_index();
    if (vi < 0) {
        s_sugg.clear(); s_sig = 0;
        // Suggestions need the vocal stem: separate the track once the
        // cache check on load has come back empty.  Once per track, so a
        // failure is reported rather than retried every frame.
        if (st == STEMS_IDLE && s_sep_requested != audio->load_count) {
            s_sep_requested = audio->load_count;
            stems_request(audio->filename, false);
            st = stems_status();
        }
        if (st == STEMS_RUNNING)
            snprintf(s_status, sizeof(s_status),
                     "Separating stems for the vocal track\xe2\x80\xa6 %d%%",
                     (int)(stems_progress() * 100.0f));
        else if (st == STEMS_CHECKING)
            snprintf(s_status, sizeof(s_status), "Looking for separated stems\xe2\x80\xa6");
        else if (st == STEMS_FAILED)
            snprintf(s_status, sizeof(s_status), "No vocal stem: %s", stems_message());
        else if (st == STEMS_READY)
            snprintf(s_status, sizeof(s_status), "The stems have no vocal track.");
        else
            snprintf(s_status, sizeof(s_status), "Waiting for the stems\xe2\x80\xa6");
        return;
    }

    const double dur = audio->duration;
    uint64_t h = 1469598103934665603ull;
    int gen = stems_generation();
    hash_bytes(&h, &gen, sizeof(gen));
    hash_bytes(&h, &dur, sizeof(dur));
    hash_bytes(&h, &audio->load_count, sizeof(audio->load_count));
    for (int i = 0; i < lm->count; i++) {
        const Lyric& ly = lm->lyrics[i];
        hash_bytes(&h, &ly.t_start, sizeof(double));
        hash_bytes(&h, &ly.t_end, sizeof(double));
        hash_bytes(&h, ly.text, strlen(ly.text));
    }
    for (double k : s_dismissed) hash_bytes(&h, &k, sizeof(k));
    if (h == s_sig) return;
    s_sig = h;
    s_sugg.clear();
    s_hover = -1;

    // Vocal stem envelope (the first use decodes the stem: a one-time stall).
    uint64_t frames = 0; uint32_t ch = 0, sr = 0;
    const float* pcm = stems_source_audio(1u << vi, &frames, &ch, &sr);
    if (!pcm || ch != 1 || sr == 0) {
        snprintf(s_status, sizeof(s_status), "Could not decode the vocal stem.");
        return;
    }
    if (pcm != s_act_pcm || gen != s_act_gen) {
        lyric_activity(pcm, frames, sr, &s_act);
        s_act_pcm = pcm;
        s_act_gen = gen;
    }

    // Placed lyrics are anchors: their spans are taken out of the singing
    // and, where the parking slots tell (lyricmap_sheet_order), the lines
    // before each in the sheet must fit before it.  The rest go around them.
    std::vector<int> sheet(lm->count > 0 ? lm->count : 1);
    lyricmap_sheet_order(lm, dur, sheet.data());
    std::vector<LyricAnchor> anchors;
    std::vector<const char*> lines;
    std::vector<int>         line_idx;
    std::vector<int>         kept_before;   // kept lines among the first k unplaced
    int n_dismissed = 0, n_unplaced = 0, n_known = 0;
    kept_before.push_back(0);
    for (int i = 0; i < lm->count; i++) {
        const Lyric& ly = lm->lyrics[i];
        if (is_unplaced(ly, dur)) {
            n_unplaced++;
            if (is_dismissed(ly.t_start)) n_dismissed++;
            else { lines.push_back(ly.text); line_idx.push_back(i); }
            kept_before.push_back((int)lines.size());
        }
    }
    for (int i = 0; i < lm->count; i++) {
        const Lyric& ly = lm->lyrics[i];
        if (is_unplaced(ly, dur)) continue;
        LyricAnchor a;
        a.t0 = ly.t_start; a.t1 = ly.t_end; a.text = ly.text;
        int sq = sheet[i];
        a.seq = (sq >= 0 && sq <= n_unplaced) ? kept_before[sq] : -1;   // dismissed lines drop out
        if (a.seq >= 0) n_known++;
        anchors.push_back(a);
    }
    if (lines.empty()) {
        snprintf(s_status, sizeof(s_status), n_dismissed ? "Nothing left to suggest."
                                                         : "Every lyric is placed.");
        return;
    }
    LyricAlignParams p;
    lyric_align_defaults(&p);
    std::vector<LyricAlignOut> out;
    double rate = lyric_align(s_act, 0.0, dur, lines, anchors, p, &out);
    for (size_t k = 0; k < lines.size(); k++) {
        if (!out[k].placed) continue;
        const Lyric& ly = lm->lyrics[line_idx[k]];
        LyricSuggestion s;
        s.key = ly.t_start;
        strncpy(s.text, ly.text, sizeof(s.text) - 1);
        s.text[sizeof(s.text) - 1] = 0;
        s.t0 = out[k].t0;
        s.t1 = out[k].t1;
        s.score = out[k].score;
        s_sugg.push_back(s);
    }
    if (s_cur_after >= 0.0) {
        int best = -1;
        for (int i = 0; i < (int)s_sugg.size(); i++)
            if (s_sugg[i].t0 >= s_cur_after - 1e-6 &&
                    (best < 0 || s_sugg[i].t0 < s_sugg[best].t0)) best = i;
        cursor_to(best);
        s_cur_after = -1.0;
    }
    int left = (int)lines.size() - (int)s_sugg.size();
    printf("[lyrics] auto: %d of %d unplaced suggested around %d placed (%d in known sheet order), "
           "%.3f s/syllable", (int)s_sugg.size(), (int)lines.size(), (int)anchors.size(), n_known, rate);
    if (!s_sugg.empty())
        printf("; next \"%s\" at %.2f-%.2f (fit %.2f)", s_sugg[0].text, s_sugg[0].t0,
               s_sugg[0].t1, s_sugg[0].score);
    printf("\n");
    if (getenv("BM_LYRICS_DEBUG"))
        for (const LyricSuggestion& sg : s_sugg)
            printf("[lyrics]   %7.2f-%7.2f  fit %.2f  %s\n", sg.t0, sg.t1, sg.score, sg.text);
    if (s_sugg.empty())
        snprintf(s_status, sizeof(s_status), "No singing found outside the placed lyrics.");
    else
        snprintf(s_status, sizeof(s_status), "%d suggested%s%s \xc2\xb7 %.2f s/syllable",
                 (int)s_sugg.size(), left > 0 ? ", rest past the singing" : "",
                 n_dismissed ? ", some dismissed" : "", rate);
}

const LyricSuggestion* ui_lyric_auto_for(const LyricMap* lm, int idx) {
    if (!s_active || idx < 0 || idx >= lm->count) return nullptr;
    const Lyric& ly = lm->lyrics[idx];
    for (const LyricSuggestion& s : s_sugg)
        if (s.key == ly.t_start && strcmp(s.text, ly.text) == 0) return &s;
    return nullptr;
}

int ui_lyric_auto_apply(LyricMap* lm, int idx) {
    const LyricSuggestion* s = ui_lyric_auto_for(lm, idx);
    if (!s) return -1;
    double t0 = s->t0, t1 = s->t1;
    int cur = ui_lyric_auto_cursor();
    if (cur >= 0 && &s_sugg[cur] == s) s_cur_after = t1;   // carry on after it
    else if (cur < 0) s_cur_set = false;
    char text[128];
    strncpy(text, lm->lyrics[idx].text, sizeof(text) - 1);
    text[sizeof(text) - 1] = 0;
    lyricmap_remove(lm, idx);
    int ni = lyricmap_add(lm, t0, t1, text);
    // The rest are recomputed against the new anchor next frame.
    s_sugg.clear();
    s_sig = 0;
    s_hover = -1;
    return ni;
}

int ui_lyric_auto_apply_all(LyricMap* lm) {
    std::vector<LyricSuggestion> all = s_sugg;
    int n = 0;
    for (const LyricSuggestion& s : all) {
        for (int i = 0; i < lm->count; i++) {
            const Lyric& ly = lm->lyrics[i];
            if (ly.t_start != s.key || strcmp(ly.text, s.text) != 0) continue;
            char text[128];
            strncpy(text, ly.text, sizeof(text) - 1);
            text[sizeof(text) - 1] = 0;
            lyricmap_remove(lm, i);
            lyricmap_add(lm, s.t0, s.t1, text);
            n++;
            break;
        }
    }
    s_sugg.clear();
    s_sig = 0;
    s_hover = -1;
    s_cur_set = false;
    return n;
}
