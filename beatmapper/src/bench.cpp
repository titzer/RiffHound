// bmbench: headless regression tool for beatmapper's inference algorithms.
//
// Takes a fully mapped track (audio + companion .txt) as ground truth, hides
// part of the map, runs a tool over the remainder and scores what it
// proposes against what was hidden.  Built to be driven by an outer script:
// every knob is settable from the command line and results print as one
// TSV line per scenario.
//
//   bmbench <audio> [--map FILE] <command> [options]
//
// Commands
//   complete   hide part of the map and re-infer it with Complete Track
//   detect     run the Beat Detector over a range and score its beats
//   shapes     train the onset-shape vocabulary and report how each shape
//              falls on beats vs half-beats
//   params     list the settable Complete Track parameters and their defaults
//
// Hiding (complete): repeatable, applied in order
//   --keep T0-T1        keep only beats/sections/chords inside (first use drops all else)
//   --drop T0-T1        drop beats/sections/chords inside
//   --keep-section N    keep only the N-th section (0-based) and what is in it
//   --drop-beats N:M    drop every N-th beat starting at M (thin the map)
//   --no-sections       drop every section (keep beats and chords)
//   --no-chords         drop every chord
//   --hide-chords T0-T1 drop chords starting inside (beats and sections stay)
//   --edge-ms MS        shift every visible section/chord edge by MS (late if > 0)
//
// Options
//   --algo NAME|IDX     gap-fill strategy (complete)
//   --set NAME=VALUE    any Complete Track parameter (see `params`); repeatable
//   --smooth2 N         run N passes of second-stage per-segment smoothing
//   --stages            after beats, accept them all and score sections, then chords
//   --tol MS            error tolerance for "hit" (default 50)
//   --tsv               one machine-readable line (header with --header)
//   --list              print every candidate
//   --debug             strategy trace on stderr
#include "complete_algo.h"
#include "onset_shape.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "miscmap.h"
#include "audio.h"
#include "beat_algo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// Parameter table
// ---------------------------------------------------------------------------
enum PType { PT_FLOAT, PT_INT, PT_BOOL };
struct PDesc { const char* name; PType type; size_t off; const char* help; };
#define PF(field, help) { #field, PT_FLOAT, offsetof(CompleteParams, field), help }
#define PI(field, help) { #field, PT_INT,   offsetof(CompleteParams, field), help }
#define PB(field, help) { #field, PT_BOOL,  offsetof(CompleteParams, field), help }
static const PDesc PARAMS[] = {
    PB(do_beats,             "run the beat stage"),
    PB(do_sections,          "run the section stage"),
    PB(do_chords,            "run the chord stage"),
    PF(gap_factor,           "interval > this x median IBI is a gap"),
    PI(template_beats,       "chunk length for unsectioned templates"),
    PI(min_template_beats,   "shortest template"),
    PF(max_warp,             "max |stretch-1| tried"),
    PI(warp_steps,           "stretch factors tried"),
    PF(warp_weight,          "score penalty per unit warp"),
    PF(jitter_beats,         "sub-beat start slack"),
    PI(lookahead_beats,      "whole beats a template may start ahead"),
    PF(lookahead_penalty,    "score cost per lookahead beat"),
    PF(beat_sim_threshold,   "min score to transfer"),
    PI(beat_algo_idx,        "BEAT_ALGOS index for onsets"),
    PI(fill_algo_idx,        "gap-fill strategy"),
    PF(rhythm_weight,        "rhythm bonus weight"),
    PF(miss_penalty,         "rhythm: expected hit missing"),
    PF(extra_penalty,        "rhythm: unexpected hit"),
    PF(det_min_bpm,          "detector min BPM"),
    PF(det_max_bpm,          "detector max BPM"),
    PF(det_threshold,        "detector onset threshold"),
    PF(det_tightness,        "detector DP tightness"),
    PF(onset_weight,         "pull toward onsets 0..1"),
    PB(refit,                "least-squares refit of each segment to its onsets"),
    PB(grid_follow,          "tempo fills follow the detector's beat grid"),
    PF(onset_window,         "onset search +- fraction of beat"),
    PF(smooth.strength,      "fill smoothing strength"),
    PI(smooth.iterations,    "fill smoothing passes"),
    PF(smooth.max_shift,     "fill smoothing max shift (s)"),
    PB(smooth.use_onsets,    "fill smoothing uses onsets"),
    PF(smooth2.strength,     "stage-2 smoothing strength"),
    PI(smooth2.iterations,   "stage-2 smoothing passes"),
    PF(smooth2.max_shift,    "stage-2 max shift (s)"),
    PB(smooth2.use_onsets,   "stage-2 uses onsets"),
    PF(smooth2.onset_weight, "stage-2 onset pull"),
    PF(smooth2.onset_window, "stage-2 onset window"),
    PI(section_granularity,  "0 measure, 1 beat"),
    PF(section_sim_threshold,"section match threshold"),
    PF(section_max_overlap,  "allowed overlap with existing sections"),
    PF(section_rhythm_weight,"rhythm vs chroma in range comparisons"),
    PB(section_discover,     "discover repeats by self-similarity"),
    PI(section_min_measures, "shortest discovered repeat unit"),
    PB(chord_runs,           "progression repeat matching"),
    PI(chord_run_beats,      "max progression template beats"),
    PF(chord_sim_threshold,  "progression match threshold"),
    PB(chord_fallback,       "beat-by-beat fallback (learned models / triads)"),
    PF(chord_transition,     "decoder: chord change cost"),
    PF(chord_measure_bonus,  "decoder: change cost waived on measure starts"),
    PF(chord_unseen_penalty, "decoder: penalty for triads not in the map (>=1 disables)"),
    PF(chord_prior_beats,    "learned model triad prior weight"),
    PB(chord_learn_rate,     "scale change cost by the map's median chord length"),
    PI(chroma.algo_idx,      "CHROMA_ALGOS index for beat chroma"),
    PF(chroma.attack_ms,     "beat chroma: skip attack ms"),
    PF(chroma.attack_frac,   "beat chroma: skip attack fraction"),
    PI(shape.k,              "timbre vocabulary size"),
    PF(shape.window_frac,    "timbre window fraction of beat"),
    PI(shape.slots_per_beat, "rhythm grid slots per beat"),
    PI(shape.bands,          "timbre spectral bands"),
    PI(shape.frames,         "timbre sub-frames"),
    PF(shape.low_weight,     "timbre low-band weight"),
    PF(shape.high_weight,    "timbre high-band weight"),
    PF(shape.onset_threshold,"timbre onset harvest threshold"),
};
static const int N_PARAMS = (int)(sizeof(PARAMS) / sizeof(PARAMS[0]));

static bool set_param(CompleteParams* p, const char* name, const char* value) {
    for (int i = 0; i < N_PARAMS; i++) {
        if (strcmp(PARAMS[i].name, name) != 0) continue;
        char* base = (char*)p + PARAMS[i].off;
        switch (PARAMS[i].type) {
        case PT_FLOAT: *(float*)base = (float)atof(value); break;
        case PT_INT:   *(int*)base   = atoi(value); break;
        case PT_BOOL:  *(bool*)base  = (atoi(value) != 0 || !strcmp(value, "true")); break;
        }
        return true;
    }
    return false;
}

static void print_params(const CompleteParams& p) {
    for (int i = 0; i < N_PARAMS; i++) {
        const char* base = (const char*)&p + PARAMS[i].off;
        switch (PARAMS[i].type) {
        case PT_FLOAT: printf("%-24s %-10g %s\n", PARAMS[i].name, *(const float*)base, PARAMS[i].help); break;
        case PT_INT:   printf("%-24s %-10d %s\n", PARAMS[i].name, *(const int*)base,   PARAMS[i].help); break;
        case PT_BOOL:  printf("%-24s %-10s %s\n", PARAMS[i].name, *(const bool*)base ? "true" : "false", PARAMS[i].help); break;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool parse_range(const char* s, double* a, double* b) {
    return sscanf(s, "%lf-%lf", a, b) == 2 && *b > *a;
}

static double median_ibi(const std::vector<double>& t) {
    if (t.size() < 2) return 0.5;
    std::vector<double> d;
    for (size_t i = 1; i < t.size(); i++) d.push_back(t[i] - t[i - 1]);
    std::sort(d.begin(), d.end());
    return d[d.size() / 2];
}

struct Truth {
    std::vector<double>  beats;
    std::vector<Section> sections;
    std::vector<MiscAnnotation> chords;
    double t_end;          // last truth beat (scores are limited to this)
};

struct BeatScore {
    int n = 0, hit = 0, bad = 0, half = 0, missed = 0;
    double mean_ms = 0, max_ms = 0;
};

// Score proposed beat times against the hidden truth beats within [lo, hi].
static BeatScore score_beats(const std::vector<double>& prop, const Truth& tr,
                             const std::vector<char>& hidden, double tol_s)
{
    BeatScore s;
    double med = median_ibi(tr.beats);
    double err = 0;
    double t_begin = tr.beats.empty() ? 0.0 : tr.beats.front();
    for (double t : prop) {
        if (t > tr.t_end + 0.5 || t < t_begin - 0.5) continue;   // outside the ground truth
        auto it = std::lower_bound(tr.beats.begin(), tr.beats.end(), t);
        double best = 1e9;
        if (it != tr.beats.end()) best = std::min(best, fabs(*it - t));
        if (it != tr.beats.begin()) best = std::min(best, fabs(*(it - 1) - t));
        err += best; s.max_ms = std::max(s.max_ms, best * 1000.0); s.n++;
        if (best <= tol_s) s.hit++; else s.bad++;
        if (best > 0.3 * med && best < 0.7 * med) s.half++;
    }
    s.mean_ms = s.n ? 1000.0 * err / s.n : 0.0;
    for (size_t i = 0; i < tr.beats.size(); i++) {
        if (!hidden[i]) continue;
        double u = tr.beats[i];
        auto it = std::lower_bound(prop.begin(), prop.end(), u);
        double best = 1e9;
        if (it != prop.end()) best = std::min(best, fabs(*it - u));
        if (it != prop.begin()) best = std::min(best, fabs(*(it - 1) - u));
        if (best > tol_s) s.missed++;
    }
    return s;
}

// First token of a chord annotation ("D D D D" -> "D").
static void chord_token(const char* text, char* out, int n) {
    int i = 0;
    while (text[i] && text[i] != ' ' && i < n - 1) { out[i] = text[i]; i++; }
    out[i] = 0;
}

// Per-beat chord accuracy over truth beats whose chord was hidden: the
// proposal (any chord candidate, or chords riding with a section) covering
// the beat must name the same chord.
static void score_chord_beats(const Truth& tr, const MiscMap& cm_visible,
                              const CompleteProposal& out, int* n_beats, int* n_ok)
{
    *n_beats = *n_ok = 0;
    for (size_t i = 0; i + 1 < tr.beats.size(); i++) {
        double t = tr.beats[i] + 1e-3;
        const MiscAnnotation* truth = nullptr;
        for (const MiscAnnotation& c : tr.chords) if (c.t_start <= t && c.t_end > t) { truth = &c; break; }
        if (!truth) continue;
        bool visible = false;
        for (int j = 0; j < cm_visible.count; j++)
            if (fabs(cm_visible.entries[j].t_start - truth->t_start) < 1e-3) visible = true;
        if (visible) continue;
        (*n_beats)++;
        const ChordProposal* prop = nullptr;
        for (const CompleteCand& c : out.cands) {
            int first = -1, n = 0;
            if (c.kind == CAND_CHORDS) { first = c.first; n = c.n; }
            else if (c.kind == CAND_SECTION && c.chord_n > 0) { first = c.chord_first; n = c.chord_n; }
            for (int k = 0; k < n; k++) {
                const ChordProposal& cp = out.chords[first + k];
                if (cp.t0 <= t && cp.t1 > t) { prop = &cp; break; }
            }
            if (prop) break;
        }
        if (!prop) continue;
        char a[32], b[32];
        chord_token(truth->text, a, sizeof(a)); chord_token(prop->text, b, sizeof(b));
        if (!strcmp(a, b)) (*n_ok)++;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
static void usage() {
    fprintf(stderr,
        "usage: bmbench <audio> [--map FILE] <complete|detect|shapes|params> [options]\n"
        "  complete: --keep T0-T1 | --drop T0-T1 | --keep-section N | --drop-beats N:M\n"
        "            --algo NAME|IDX  --set NAME=VALUE  --smooth2 N  --stages  --tol MS\n"
        "            --tsv [--header]  --list  --debug\n"
        "  detect:   --region T0-T1 [--set det_*=...]  --tol MS  --tsv\n"
        "  shapes:   [--set shape.*=...]\n");
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    const char* audio_path = argv[1];
    std::string map_path;
    int ai = 2;
    if (!strcmp(argv[ai], "--map") && ai + 1 < argc) { map_path = argv[ai + 1]; ai += 2; }
    if (ai >= argc) { usage(); return 2; }
    std::string cmd = argv[ai++];

    CompleteParams p; complete_params_defaults(&p);
    if (cmd == "params") { print_params(p); return 0; }

    struct Hide { int kind; double a, b; int n, m; };   // 0 keep, 1 drop, 2 keep-section, 3 drop-beats
    std::vector<Hide> hides;
    int smooth2 = 0; bool stages = false, tsv = false, header = false, list = false;
    bool no_sections = false, no_chords = false, dump = false;
    double hc0 = 0, hc1 = 0; bool hide_chords = false;
    double edge_ms = 0.0;
    double tol_ms = 50.0;
    double reg0 = 0, reg1 = 0; bool have_reg = false;

    for (; ai < argc; ai++) {
        const char* a = argv[ai];
        const char* v = (ai + 1 < argc) ? argv[ai + 1] : nullptr;
        if (!strcmp(a, "--keep") && v)        { Hide h = { 0, 0, 0, 0, 0 }; if (!parse_range(v, &h.a, &h.b)) { fprintf(stderr, "bad range %s\n", v); return 2; } hides.push_back(h); ai++; }
        else if (!strcmp(a, "--drop") && v)   { Hide h = { 1, 0, 0, 0, 0 }; if (!parse_range(v, &h.a, &h.b)) { fprintf(stderr, "bad range %s\n", v); return 2; } hides.push_back(h); ai++; }
        else if (!strcmp(a, "--keep-section") && v) { Hide h = { 2, 0, 0, atoi(v), 0 }; hides.push_back(h); ai++; }
        else if (!strcmp(a, "--drop-beats") && v)   { Hide h = { 3, 0, 0, 0, 0 }; sscanf(v, "%d:%d", &h.n, &h.m); hides.push_back(h); ai++; }
        else if (!strcmp(a, "--region") && v) { if (!parse_range(v, &reg0, &reg1)) return 2; have_reg = true; ai++; }
        else if (!strcmp(a, "--algo") && v) {
            bool ok = false;
            for (int i = 0; i < complete_fill_algo_count(); i++)
                if (!strcasecmp(complete_fill_algo_name(i), v) || atoi(v) == i) { p.fill_algo_idx = i; ok = true; }
            if (!ok && isdigit((unsigned char)v[0])) { p.fill_algo_idx = atoi(v); ok = true; }
            if (!ok) { fprintf(stderr, "unknown algo %s\n", v); return 2; }
            ai++;
        }
        else if (!strcmp(a, "--set") && v) {
            char name[64] = {}, val[64] = {};
            if (sscanf(v, "%63[^=]=%63s", name, val) != 2 || !set_param(&p, name, val)) {
                fprintf(stderr, "bad --set %s (see `bmbench x params`)\n", v); return 2;
            }
            ai++;
        }
        else if (!strcmp(a, "--smooth2") && v) { smooth2 = atoi(v); ai++; }
        else if (!strcmp(a, "--tol") && v)     { tol_ms = atof(v); ai++; }
        else if (!strcmp(a, "--stages"))  stages = true;
        else if (!strcmp(a, "--no-sections")) no_sections = true;
        else if (!strcmp(a, "--no-chords"))   no_chords = true;
        else if (!strcmp(a, "--edge-ms") && v) { edge_ms = atof(v); ai++; }
        else if (!strcmp(a, "--hide-chords") && v) { if (!parse_range(v, &hc0, &hc1)) return 2; hide_chords = true; ai++; }
        else if (!strcmp(a, "--tsv"))     tsv = true;
        else if (!strcmp(a, "--header"))  header = true;
        else if (!strcmp(a, "--list"))    list = true;
        else if (!strcmp(a, "--dump"))    dump = true;
        else if (!strcmp(a, "--debug"))   setenv("COMPLETE_DEBUG", "1", 1);
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
    }

    // --- load -------------------------------------------------------------
    BeatMap bm; SectionMap sm; LyricMap lm; MiscMap mm, cm;
    beatmap_init(&bm); sectionmap_init(&sm); lyricmap_init(&lm);
    miscmap_init(&mm); miscmap_init(&cm, "chord");
    if (map_path.empty()) {
        char buf[512]; beatmap_path_for_audio(audio_path, buf, sizeof(buf)); map_path = buf;
    }
    if (!beatmap_load(&bm, &sm, &lm, &mm, &cm, map_path.c_str())) {
        fprintf(stderr, "cannot load map %s\n", map_path.c_str()); return 1;
    }
    float* pcm = nullptr; uint64_t nf = 0; uint32_t sr = 0;
    if (!audio_decode_pcm(audio_path, &pcm, &nf, &sr)) {
        fprintf(stderr, "cannot decode %s\n", audio_path); return 1;
    }
    AudioPcm au = { pcm, nf, 1, sr };
    double duration = (double)nf / sr;

    Truth tr;
    for (int i = 0; i < bm.count; i++) tr.beats.push_back(bm.beats[i].time);
    tr.sections.assign(sm.sections, sm.sections + sm.count);
    tr.chords.assign(cm.entries, cm.entries + cm.count);
    tr.t_end = tr.beats.empty() ? duration : tr.beats.back();
    double tol_s = tol_ms / 1000.0;

    // --- shapes -----------------------------------------------------------
    if (cmd == "shapes") {
        ShapeAnalysis* sa = shape_track();
        shape_analysis_ensure(sa, au, &bm, duration, p.shape, p.beat_algo_idx);
        printf("%d onsets, window %.0f ms, vocabulary %s\n", (int)sa->onset_t.size(), sa->win * 1000.0,
               sa->vocab.valid ? "ok" : "none");
        if (!sa->vocab.valid) return 0;
        double med = median_ibi(tr.beats);
        for (int c = 0; c < sa->vocab.k; c++) {
            int on = 0, half = 0, other = 0; double conf = 0; int n = 0;
            for (size_t i = 0; i < sa->onset_t.size(); i++) {
                if (sa->onset_shape[i] != c) continue;
                conf += sa->onset_soft[i * sa->vocab.k + c]; n++;
                double t = sa->onset_t[i];
                auto it = std::lower_bound(tr.beats.begin(), tr.beats.end(), t);
                if (it == tr.beats.begin() || it == tr.beats.end()) continue;
                double ph = (t - *(it - 1)) / (*it - *(it - 1));
                if (ph < 0.1 || ph > 0.9) on++; else if (ph > 0.4 && ph < 0.6) half++; else other++;
            }
            printf("shape %d: %4d hits  %.0f dB  conf %.2f  on-beat %4d  half-beat %4d  other %4d\n",
                   c, n, sa->vocab.energy[c], n ? conf / n : 0.0, on, half, other);
        }
        (void)med;
        return 0;
    }

    // --- detect -----------------------------------------------------------
    if (cmd == "detect") {
        if (!have_reg) { fprintf(stderr, "detect needs --region\n"); return 2; }
        static AutoBeatList ab; autobeat_init(&ab);
        BeatAlgoParams bp = {};
        bp.min_bpm = p.det_min_bpm; bp.max_bpm = p.det_max_bpm;
        bp.onset_threshold = p.det_threshold; bp.dp_tightness = p.det_tightness;
        BEAT_ALGOS[p.beat_algo_idx].fn(pcm, nf, 1, sr, reg0, reg1, &bp, &ab);
        std::vector<double> prop(ab.beat_times, ab.beat_times + ab.beat_count);
        std::vector<char> hidden(tr.beats.size(), 0);
        for (size_t i = 0; i < tr.beats.size(); i++) hidden[i] = tr.beats[i] >= reg0 && tr.beats[i] <= reg1;
        BeatScore s = score_beats(prop, tr, hidden, tol_s);
        if (tsv) {
            if (header) printf("cmd\tn\thit\tbad\thalf\tmissed\tmean_ms\tmax_ms\tbpm\n");
            printf("detect\t%d\t%d\t%d\t%d\t%d\t%.1f\t%.1f\t%.1f\n", s.n, s.hit, s.bad, s.half, s.missed, s.mean_ms, s.max_ms, ab.estimated_bpm);
        } else {
            printf("detect %.1f-%.1f: %d beats (~%.1f BPM), %d within %.0f ms, %d off, %d half-beat, %d truth missed, mean %.1f ms max %.1f ms\n",
                   reg0, reg1, s.n, ab.estimated_bpm, s.hit, tol_ms, s.bad, s.half, s.missed, s.mean_ms, s.max_ms);
        }
        return 0;
    }

    if (cmd != "complete") { usage(); return 2; }

    // --- hide part of the map ---------------------------------------------
    std::vector<char> hidden(tr.beats.size(), 0);
    bool first_keep = true;
    auto in_range = [](double t, double a, double b) { return t >= a - 1e-6 && t <= b + 1e-6; };
    for (const Hide& h : hides) {
        if (h.kind == 0 || h.kind == 2) {
            double a = h.a, b = h.b;
            if (h.kind == 2) {
                if (h.n < 0 || h.n >= (int)tr.sections.size()) { fprintf(stderr, "no section %d\n", h.n); return 2; }
                a = tr.sections[h.n].t_start; b = tr.sections[h.n].t_end;
            }
            if (first_keep) {   // first keep drops everything else
                for (size_t i = 0; i < tr.beats.size(); i++) hidden[i] = 1;
                for (int i = sm.count - 1; i >= 0; i--) sectionmap_remove(&sm, i);
                for (int i = cm.count - 1; i >= 0; i--) miscmap_remove(&cm, i);
                first_keep = false;
            }
            for (size_t i = 0; i < tr.beats.size(); i++) if (in_range(tr.beats[i], a, b)) hidden[i] = 0;
            for (const Section& s : tr.sections)
                if (s.t_start >= a - 1e-3 && s.t_end <= b + 1e-3) {
                    int idx = sectionmap_add(&sm, s.t_start, s.t_end, s.kind, s.label);
                    if (idx >= 0) { sm.sections[idx].ts_num = s.ts_num; sm.sections[idx].ts_den = s.ts_den; }
                }
            for (const MiscAnnotation& c : tr.chords)
                if (c.t_start >= a - 1e-3 && c.t_start <= b) miscmap_add(&cm, c.t_start, c.t_end, c.text);
        } else if (h.kind == 1) {
            for (size_t i = 0; i < tr.beats.size(); i++) if (in_range(tr.beats[i], h.a, h.b)) hidden[i] = 1;
            for (int i = sm.count - 1; i >= 0; i--)
                if (sm.sections[i].t_end > h.a && sm.sections[i].t_start < h.b) sectionmap_remove(&sm, i);
            for (int i = cm.count - 1; i >= 0; i--)
                if (in_range(cm.entries[i].t_start, h.a, h.b)) miscmap_remove(&cm, i);
        } else if (h.kind == 3 && h.n > 0) {
            for (size_t i = 0; i < tr.beats.size(); i++) if (((int)i - h.m) % h.n == 0 && (int)i >= h.m) hidden[i] = 1;
        }
    }
    // Nudge every visible section/chord edge (simulates hand-placed edges
    // that sit a little after their beat)
    if (edge_ms != 0.0) {
        for (int i = 0; i < sm.count; i++) { sm.sections[i].t_start += edge_ms / 1000.0; sm.sections[i].t_end += edge_ms / 1000.0; }
        for (int i = 0; i < cm.count; i++) { cm.entries[i].t_start += edge_ms / 1000.0; cm.entries[i].t_end += edge_ms / 1000.0; }
    }
    if (no_sections) for (int i = sm.count - 1; i >= 0; i--) sectionmap_remove(&sm, i);
    if (no_chords)   for (int i = cm.count - 1; i >= 0; i--) miscmap_remove(&cm, i);
    if (hide_chords) for (int i = cm.count - 1; i >= 0; i--)
        if (cm.entries[i].t_start >= hc0 - 1e-6 && cm.entries[i].t_start <= hc1) miscmap_remove(&cm, i);
    // Rebuild the beatmap from the unhidden truth
    while (bm.count > 0) beatmap_remove(&bm, bm.count - 1);
    int n_hidden = 0;
    for (size_t i = 0; i < tr.beats.size(); i++) { if (hidden[i]) n_hidden++; else beatmap_add(&bm, tr.beats[i]); }

    CompleteInputs in = {};
    in.beatmap = &bm; in.sectionmap = &sm; in.chordmap = &cm;
    in.audio = au; in.duration = duration;
    in.has_region = have_reg; in.region_start = reg0; in.region_end = reg1;

    BeatChromaCache cache; CompleteProposal out;
    bool want_sections = p.do_sections, want_chords = p.do_chords;
    p.do_sections = p.do_chords = false;
    complete_run(in, p, &cache, &out);
    char beat_status[160]; strncpy(beat_status, out.status, sizeof(beat_status)); beat_status[159] = 0;
    for (int k = 0; k < smooth2; k++) complete_smooth_segments(&out, &bm, p.smooth2, false);

    std::vector<double> prop = out.beat_times;
    std::sort(prop.begin(), prop.end());
    BeatScore s = score_beats(prop, tr, hidden, tol_s);

    int n_transfer = 0, n_tempo = 0;
    for (const CompleteCand& c : out.cands)
        if (c.kind == CAND_BEATS) { if (!strcmp(c.source, "tempo")) n_tempo++; else n_transfer++; }

    // Stage 2/3: accept all beats, score sections, accept them, score chords
    int sec_found = 0, sec_true = 0, sec_false = 0, ch_found = 0, ch_true = 0, ch_text_ok = 0;
    int cb_n = 0, cb_ok = 0;
    MiscMap cm_visible; miscmap_init(&cm_visible, "chord");
    for (int i = 0; i < cm.count; i++) miscmap_add(&cm_visible, cm.entries[i].t_start, cm.entries[i].t_end, cm.entries[i].text);
    if (stages && (want_sections || want_chords)) {
        for (double t : out.beat_times) beatmap_add(&bm, t);
        p.do_beats = false; p.do_sections = true; p.do_chords = false;
        complete_run(in, p, &cache, &out);
        std::vector<char> sec_hidden(tr.sections.size(), 1);
        for (size_t i = 0; i < tr.sections.size(); i++)
            for (int j = 0; j < sm.count; j++)
                if (fabs(sm.sections[j].t_start - tr.sections[i].t_start) < 1e-3) sec_hidden[i] = 0;
        for (size_t i = 0; i < tr.sections.size(); i++) if (sec_hidden[i]) sec_true++;
        if (list) for (const CompleteCand& c : out.cands) printf("  [%d] %.2f %s\n", c.kind, c.score, c.desc);
        CompleteProposal sec_stage = out;     // keep the section stage's chords for scoring
        for (const CompleteCand& c : out.cands) {
            if (c.kind != CAND_SECTION) continue;
            if (c.t0 > tr.t_end + 0.5) continue;      // beyond the ground truth: unknowable
            bool ok = false;
            for (size_t i = 0; i < tr.sections.size(); i++)
                if (sec_hidden[i] && fabs(tr.sections[i].t_start - c.t0) < 0.25 && fabs(tr.sections[i].t_end - c.t1) < 0.25 &&
                    tr.sections[i].kind == c.sec_kind) { ok = true; break; }
            if (ok) { sec_found++; } else sec_false++;
            int idx = sectionmap_add(&sm, c.t0, c.t1, c.sec_kind, c.label);
            if (idx >= 0) sm.sections[idx].ts_num = c.ts_num;
            for (int i = 0; i < c.chord_n; i++) {        // chords that rode along with the section
                const ChordProposal& cp = out.chords[c.chord_first + i];
                miscmap_add(&cm, cp.t0, cp.t1, cp.text);
                if (cp.t0 > tr.t_end + 0.5) continue;
                ch_found++;
                for (const MiscAnnotation& t : tr.chords)
                    if (fabs(t.t_start - cp.t0) < 0.15) { if (!strcmp(t.text, cp.text)) ch_text_ok++; break; }
            }
        }
        if (want_chords) {
            p.do_sections = false; p.do_chords = true;
            complete_run(in, p, &cache, &out);
            {
                // Score both stages' chords: those that rode with sections and the rest
                CompleteProposal both = out;
                int base = (int)both.chords.size();
                both.chords.insert(both.chords.end(), sec_stage.chords.begin(), sec_stage.chords.end());
                for (const CompleteCand& c : sec_stage.cands)
                    if (c.kind == CAND_SECTION && c.chord_n > 0) {
                        CompleteCand cc = c; cc.chord_first += base; both.cands.push_back(cc);
                    }
                score_chord_beats(tr, cm_visible, both, &cb_n, &cb_ok);
            }
            for (const MiscAnnotation& c : tr.chords) {
                bool present = false;
                for (int j = 0; j < cm.count; j++) if (fabs(cm.entries[j].t_start - c.t_start) < 1e-3) present = true;
                if (!present) ch_true++;
            }
            ch_true += ch_found;   // hidden chords the section stage already restored
            for (const ChordProposal& cp : out.chords) {
                if (cp.t0 > tr.t_end + 0.5) continue;
                ch_found++;
                for (const MiscAnnotation& c : tr.chords)
                    if (fabs(c.t_start - cp.t0) < 0.15) { if (!strcmp(c.text, cp.text)) ch_text_ok++; break; }
            }
        }
    }

    if (dump) {
        for (size_t i = 0; i < out.beat_times.size(); i++) {
            double t = out.beat_times[i];
            auto it = std::lower_bound(tr.beats.begin(), tr.beats.end(), t);
            double best = 1e9;
            if (it != tr.beats.end()) best = std::min(best, *it - t);
            if (it != tr.beats.begin() && fabs(*(it - 1) - t) < fabs(best)) best = *(it - 1) - t;
            printf("  %8.3f  truth %+7.3f  conf %.2f\n", t, best, out.beat_conf[i]);
        }
    }
    if (list) {
        for (const CompleteCand& c : out.cands) {
            printf("  [%d] %.2f %s", c.kind, c.score, c.desc);
            if (c.kind == CAND_BEATS && c.n > 0) {
                // Per-segment accuracy against the truth
                std::vector<double> seg(out.beat_times.begin() + c.first, out.beat_times.begin() + c.first + c.n);
                std::sort(seg.begin(), seg.end());
                BeatScore ss = score_beats(seg, tr, std::vector<char>(tr.beats.size(), 0), tol_s);
                if (ss.n > 0) printf("   => %d/%d hit, %d half, mean %.0f ms", ss.hit, ss.n, ss.half, ss.mean_ms);
            }
            printf("\n");
        }
    }
    const char* algo = complete_fill_algo_name(p.fill_algo_idx);
    if (tsv) {
        if (header) printf("algo\thidden\tn\thit\tbad\thalf\tmissed\tmean_ms\tmax_ms\ttransfers\ttempo_runs\tsec_found\tsec_true\tsec_false\tch_found\tch_true\tch_text_ok\tcb_ok\tcb_n\n");
        printf("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%.1f\t%.1f\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",
               algo, n_hidden, s.n, s.hit, s.bad, s.half, s.missed, s.mean_ms, s.max_ms,
               n_transfer, n_tempo, sec_found, sec_true, sec_false, ch_found, ch_true, ch_text_ok, cb_ok, cb_n);
    } else {
        printf("%s: hid %d of %d beats; %s\n", algo, n_hidden, (int)tr.beats.size(), beat_status);
        printf("  beats: %d proposed in truth range, %d within %.0f ms, %d off, %d half-beat, %d hidden missed, mean %.1f ms, max %.1f ms  (%d transfers, %d tempo runs)\n",
               s.n, s.hit, tol_ms, s.bad, s.half, s.missed, s.mean_ms, s.max_ms, n_transfer, n_tempo);
        if (stages) {
            printf("  sections: %d of %d hidden recovered (edges within 0.25 s), %d spurious\n", sec_found, sec_true, sec_false);
            if (want_chords) {
                printf("  chords: %d proposed for %d hidden, %d with the right name at the right time\n", ch_found, ch_true, ch_text_ok);
                printf("  chord beats: %d of %d hidden beats carry the right chord (%.0f%%)\n", cb_ok, cb_n, cb_n ? 100.0 * cb_ok / cb_n : 0.0);
            }
        }
    }
    audio_free_pcm(pcm);
    return 0;
}
