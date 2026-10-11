#include "lyricmap.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>

void lyricmap_init(LyricMap* lm) {
    lm->lyrics       = nullptr;
    lm->count        = 0;
    lm->capacity     = 0;
    lm->dirty        = false;
    lm->selected_idx = -1;
}

void lyricmap_shutdown(LyricMap* lm) {
    free(lm->lyrics);
    lm->lyrics   = nullptr;
    lm->count    = 0;
    lm->capacity = 0;
}

void lyricmap_clear(LyricMap* lm) {
    lm->count        = 0;
    lm->dirty        = false;
    lm->selected_idx = -1;
}

int lyricmap_add(LyricMap* lm, double t_start, double t_end, const char* text) {
    if (t_end < t_start) { double tmp = t_start; t_start = t_end; t_end = tmp; }

    if (lm->count >= lm->capacity) {
        int new_cap = (lm->capacity == 0) ? 16 : lm->capacity * 2;
        Lyric* tmp = (Lyric*)realloc(lm->lyrics, new_cap * sizeof(Lyric));
        if (!tmp) return -1;
        lm->lyrics   = tmp;
        lm->capacity = new_cap;
    }

    // Sorted insertion by t_start
    int pos = lm->count;
    for (int i = 0; i < lm->count; i++) {
        if (lm->lyrics[i].t_start > t_start) { pos = i; break; }
    }

    memmove(lm->lyrics + pos + 1, lm->lyrics + pos,
            (lm->count - pos) * sizeof(Lyric));

    Lyric& ly = lm->lyrics[pos];
    ly.t_start  = t_start;
    ly.t_end    = t_end;
    ly.selected = false;
    ly.text[0]  = '\0';
    if (text && text[0])
        strncpy(ly.text, text, sizeof(ly.text) - 1);
    lm->count++;
    lm->dirty = true;

    // Keep selected_idx valid after insertion
    if (lm->selected_idx >= pos) lm->selected_idx++;
    return pos;
}

void lyricmap_remove(LyricMap* lm, int idx) {
    if (idx < 0 || idx >= lm->count) return;
    memmove(lm->lyrics + idx, lm->lyrics + idx + 1,
            (lm->count - idx - 1) * sizeof(Lyric));
    lm->count--;
    lm->dirty = true;
    if (lm->selected_idx == idx)     lm->selected_idx = -1;
    else if (lm->selected_idx > idx) lm->selected_idx--;
}

// A split at a comma, semicolon or period ("we were young,| and free" or
// "young|; and free") breaks the line there: the punctuation (a whole run of
// it, so "..." goes too) and the spaces around it go, and the next line
// starts with a capital ("we were young" / "And free").  A split at a space
// ("we were young| and free") does the same minus the punctuation; a split
// inside a word is left alone.
static bool is_line_break_punct(char c) { return c == ',' || c == ';' || c == '.'; }

static void tidy_split(char* a, char* b) {
    int la0 = (int)strlen(a);
    int la = la0;
    while (la > 0 && a[la - 1] == ' ') la--;
    const char* rb = b;
    while (*rb == ' ') rb++;
    bool punct_a = la > 0 && is_line_break_punct(a[la - 1]);
    bool punct_b = is_line_break_punct(*rb);
    bool space   = la < la0 || rb > b;
    if (!punct_a && !punct_b && !space) return;
    if (punct_a)      while (la > 0 && is_line_break_punct(a[la - 1])) la--;
    else if (punct_b) while (is_line_break_punct(*rb)) rb++;
    while (la > 0 && a[la - 1] == ' ') la--;
    a[la] = 0;
    while (*rb == ' ') rb++;
    memmove(b, rb, strlen(rb) + 1);
    if (b[0] >= 'a' && b[0] <= 'z') b[0] = (char)(b[0] - 'a' + 'A');
}

void lyricmap_split(LyricMap* lm, int idx, int cursor_pos, int* new_sel,
                    double audio_dur) {
    if (idx < 0 || idx >= lm->count) return;

    // Save lyric data before removal
    double t0 = lm->lyrics[idx].t_start;
    double t1 = lm->lyrics[idx].t_end;
    char   saved[128];
    strncpy(saved, lm->lyrics[idx].text, sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';

    int len = (int)strlen(saved);
    if (cursor_pos < 0)   cursor_pos = 0;
    if (cursor_pos > len) cursor_pos = len;

    // Build the two text halves
    char text_a[128] = {}, text_b[128] = {};
    int la = cursor_pos < 127 ? cursor_pos : 127;
    int lb = len - cursor_pos < 127 ? len - cursor_pos : 127;
    if (la > 0) strncpy(text_a, saved,              la);
    if (lb > 0) strncpy(text_b, saved + cursor_pos, lb);
    tidy_split(text_a, text_b);

    // Placed lyrics start before audio_dur; unplaced ones are parked at or
    // beyond it.  The lyric is the last placed one when nothing placed
    // follows it (the array is sorted by t_start).
    bool placed      = (audio_dur > 0.0) && (t0 < audio_dur - 1e-9);
    bool last_placed = placed &&
        (idx + 1 >= lm->count || lm->lyrics[idx + 1].t_start >= audio_dur - 1e-9);

    double ta0, ta1, tb0, tb1;
    if (last_placed) {
        // Splitting the lyric just recorded: its timing is right for the sung
        // line, so the first half keeps the whole interval and the second half
        // becomes the next unplaced lyric.  Park it in the sliver just below
        // audio_dur -- still unplaced, but sorting ahead of every parked lyric
        // (halfway between the placed boundary and the first parked start, so
        // repeated splits keep queueing up in order).
        double lo = audio_dur - 1e-9, hi = audio_dur;
        if (idx + 1 < lm->count && lm->lyrics[idx + 1].t_start < hi)
            hi = lm->lyrics[idx + 1].t_start;
        ta0 = t0;                    ta1 = t1;
        tb0 = lo + (hi - lo) * 0.5;  tb1 = tb0 + 0.001;
    } else if (t1 - t0 > 1e-4) {
        // Split the time range at the midpoint.
        double t_mid = (t0 + t1) * 0.5;
        ta0 = t0;    ta1 = t_mid;
        tb0 = t_mid; tb1 = t1;
    } else {
        // Zero/tiny range (unplaced lyrics): the second half goes halfway to
        // the next slot, so it sorts right after the first and the slot
        // numbering (lyricmap_sheet_order) sees no hole or collision.
        double next = t0 + LYRIC_SLOT;
        if (idx + 1 < lm->count && lm->lyrics[idx + 1].t_start > t0 &&
                lm->lyrics[idx + 1].t_start < next)
            next = lm->lyrics[idx + 1].t_start;
        ta0 = t0;                    ta1 = t0 + 1e-6;
        tb0 = t0 + (next - t0) * 0.5; tb1 = tb0 + 1e-6;
    }

    lyricmap_remove(lm, idx);
    lyricmap_add(lm, ta0, ta1, text_a);
    int ib = lyricmap_add(lm, tb0, tb1, text_b);
    if (new_sel) *new_sel = ib;
}

// --- parking slots ------------------------------------------------------------

double lyricmap_next_slot(const LyricMap* lm, double dur) {
    double t = dur;
    for (int i = 0; i < lm->count; i++)
        if (lm->lyrics[i].t_start >= dur && lm->lyrics[i].t_start + LYRIC_SLOT > t)
            t = lm->lyrics[i].t_start + LYRIC_SLOT;
    // Back onto the whole-slot grid (a split half sits between slots).
    return dur + ceil((t - dur) / LYRIC_SLOT - 1e-3) * LYRIC_SLOT;
}

void lyricmap_remove_parked(LyricMap* lm, int idx, double dur) {
    if (idx < 0 || idx >= lm->count) return;
    double t = lm->lyrics[idx].t_start;
    // The next parked lyric takes over this one's slot and everything after
    // moves down with it, so the deletion leaves no hole that would read as a
    // placed lyric.  Split halves (closer than a slot) shift by less.
    double shift = 0.0;
    if (t >= dur && idx + 1 < lm->count) {
        double gap = lm->lyrics[idx + 1].t_start - t;
        shift = gap < LYRIC_SLOT ? gap : LYRIC_SLOT;
    }
    lyricmap_remove(lm, idx);
    if (shift > 0.0)
        for (int i = idx; i < lm->count; i++) {
            lm->lyrics[i].t_start -= shift;
            lm->lyrics[i].t_end   -= shift;
        }
}

// Whole-slot number of a parked lyric (split halves share their origin's).
// `phase` is how far the whole slots sit off the grid, in slots: the
// duration can come out a few samples different from when the lyrics were
// parked (another decoder, a re-encoded file).
static long slot_of(double t, double dur, double phase) {
    return (long)floor((t - dur) / LYRIC_SLOT - phase + 1e-3);
}

static double slot_phase(const LyricMap* lm, double dur) {
    std::vector<double> d;
    for (int i = 0; i < lm->count; i++) {
        double x = (lm->lyrics[i].t_start - dur) / LYRIC_SLOT;
        if (x < -0.5) continue;
        double off = x - floor(x + 0.5);
        if (fabs(off) < 0.3) d.push_back(off);     // split halves sit near +-0.5
    }
    if (d.empty()) return 0.0;
    std::sort(d.begin(), d.end());
    return d[d.size() / 2];
}

// seq as lyricmap_sheet_order; slot: the parking slot each placed lyric
// came from (-1 unknown).
static void sheet_core(const LyricMap* lm, double dur, int* seq, long* slot) {
    std::vector<int> placed;               // indices, in time order
    std::vector<int> holes;                // each hole: unplaced lines before it
    std::vector<long> hole_slot;           // ...and its slot number
    int n_unplaced = 0;
    long prev_slot = -1;                   // slots start at 0 (t = dur)
    const double phase = slot_phase(lm, dur);
    for (int i = 0; i < lm->count; i++) {
        const Lyric& ly = lm->lyrics[i];
        seq[i] = -1;
        slot[i] = -1;
        if (ly.t_start < dur - 1e-9) { placed.push_back(i); continue; }
        if (ly.t_start >= dur) {
            // Holes before the first slot come before everything unplaced,
            // including lines parked just below dur: those were split off
            // the last placed lyric, so they follow it in the sheet.
            long s = slot_of(ly.t_start, dur, phase);
            int before = prev_slot < 0 ? 0 : n_unplaced;
            for (long h = prev_slot + 1; h < s; h++) { holes.push_back(before); hole_slot.push_back(h); }
            if (s > prev_slot) prev_slot = s;
        }
        n_unplaced++;
    }
    const int m = (int)placed.size(), r = (int)holes.size();
    if (m == 0) return;
    if (n_unplaced == 0) {                 // everything placed: the sheet is time order
        for (int k = 0; k < m; k++) { seq[placed[k]] = 0; slot[placed[k]] = k; }
        return;
    }
    if (r == m) {                          // the usual case: paste, then place
        for (int k = 0; k < m; k++) { seq[placed[k]] = holes[k]; slot[placed[k]] = hole_slot[k]; }
        return;
    }
    // Lyrics placed from the end of the list leave no visible hole (nothing
    // parked after them), drawn-in lyrics never had a slot, and old files
    // may predate slot bookkeeping.  Match placed lyrics to holes in order,
    // pairing those whose position in the sheet, guessed from time, is
    // closest; unmatched placed lyrics stay unknown.  A hole past the last
    // parked slot stands for everything placed from the end.
    holes.push_back(n_unplaced);
    hole_slot.push_back(-1);               // past the last slot: numbered below
    const int R = (int)holes.size();
    const double N = n_unplaced + m;
    std::vector<double> want(m), have(R);
    for (int k = 0; k < m; k++) {
        const Lyric& ly = lm->lyrics[placed[k]];
        want[k] = 0.5 * (ly.t_start + ly.t_end) / dur;
    }
    for (int j = 0; j < R; j++) have[j] = (holes[j] + (j < r ? j : m - 1)) / N;
    const double SKIP = 0.25, INF = 1e30;
    // f[k][j]: first k placed lyrics against first j holes.  The trailing
    // hole may take any number of lyrics.
    std::vector<double> f((size_t)(m + 1) * (R + 1), INF);
    std::vector<char>   how((size_t)(m + 1) * (R + 1), 0);
    auto at = [R](int k, int j) { return (size_t)k * (R + 1) + j; };
    f[at(0, 0)] = 0.0;
    for (int k = 0; k <= m; k++)
        for (int j = 0; j <= R; j++) {
            double v = f[at(k, j)];
            if (v >= INF) continue;
            if (k < m && j < R) {                         // match k with hole j
                double c = v + fabs(want[k] - have[j]);
                int nj = (j == R - 1) ? j : j + 1;         // the end hole stays open
                if (c < f[at(k + 1, nj)]) { f[at(k + 1, nj)] = c; how[at(k + 1, nj)] = (nj == j) ? 4 : 1; }
            }
            if (k < m && v + SKIP < f[at(k + 1, j)]) { f[at(k + 1, j)] = v + SKIP; how[at(k + 1, j)] = 2; }
            if (j < R && v + SKIP < f[at(k, j + 1)]) { f[at(k, j + 1)] = v + SKIP; how[at(k, j + 1)] = 3; }
        }
    int k = m, j = R;
    if (f[at(m, R - 1)] < f[at(m, R)]) j = R - 1;
    while (k > 0 || j > 0) {
        char h = how[at(k, j)];
        if (h == 1)      { seq[placed[k - 1]] = holes[j - 1]; slot[placed[k - 1]] = hole_slot[j - 1]; k--; j--; }
        else if (h == 4) { seq[placed[k - 1]] = holes[j];     k--; }
        else if (h == 2) { k--; }
        else if (h == 3) { j--; }
        else break;
    }
    // Lyrics placed from past the last parked slot: the slots after it, in
    // time order.
    long next = prev_slot + 1;
    for (int k : placed)
        if (seq[k] == n_unplaced && slot[k] < 0) slot[k] = next++;
}

void lyricmap_sheet_order(const LyricMap* lm, double dur, int* seq) {
    std::vector<long> slot(lm->count > 0 ? lm->count : 1);
    sheet_core(lm, dur, seq, slot.data());
}

void lyricmap_unplace(LyricMap* lm, const std::vector<int>& idxs, double dur) {
    if (idxs.empty()) return;
    std::vector<int>  seq(lm->count);
    std::vector<long> slot(lm->count);
    sheet_core(lm, dur, seq.data(), slot.data());
    // Remember what goes where, then re-add (indices shift as we go).
    struct Move { double t; char text[128]; };
    std::vector<Move> moves;
    std::vector<int> sorted = idxs;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    double tail = lyricmap_next_slot(lm, dur);
    for (int i = 0; i < lm->count; i++)          // past any slot handed out too
        if (slot[i] >= 0 && dur + (slot[i] + 1) * LYRIC_SLOT > tail)
            tail = dur + (slot[i] + 1) * LYRIC_SLOT;
    for (int i : sorted) {
        if (i < 0 || i >= lm->count || lm->lyrics[i].t_start >= dur - 1e-9) continue;
        Move mv;
        if (slot[i] >= 0) mv.t = dur + slot[i] * LYRIC_SLOT;
        else            { mv.t = tail; tail += LYRIC_SLOT; }   // order unknown: at the end
        strncpy(mv.text, lm->lyrics[i].text, sizeof(mv.text) - 1);
        mv.text[sizeof(mv.text) - 1] = 0;
        moves.push_back(mv);
    }
    for (int k = (int)sorted.size() - 1; k >= 0; k--) {
        int i = sorted[k];
        if (i >= 0 && i < lm->count && lm->lyrics[i].t_start < dur - 1e-9) lyricmap_remove(lm, i);
    }
    for (const Move& mv : moves) lyricmap_add(lm, mv.t, mv.t + 1e-6, mv.text);
}
