// bmbench: headless benchmark and tuning harness for beatmapper's inference.
//
// A fully mapped track is treated as ground truth (the annotations are good,
// not perfect: metrics use tolerant matching -- 50/90 ms for beats, 0.25 s
// for section edges, first-token names for chords -- and 100 % is not a
// meaningful target).  The harness hides chosen layers of the map over chosen
// spans, re-infers them in a chosen stage order, and scores what came back.
//
//   bmbench <audio-or-directory> [--map FILE] <command> [options]
//
// Commands
//   suite      the systematic scenario matrix; the one to run on a track (or
//              a directory of tracks) to see where inference stands
//   complete   one scenario, every knob on the command line
//   detect     score the Beat Detector over a range
//   shapes     train the onset-shape vocabulary and report it
//   params     list every settable parameter
//
// Every parameter of CompleteParams is settable with --set NAME=VALUE, so an
// outer script can sweep anything `params` lists.  The inference stages are
// behind tables (BEAT_FILL_ALGOS, CHROMA_ALGOS, ...), which is where external
// or learned models will plug in; the harness only names them by index.
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
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
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
    PB(section_partition,    "partition-DP section inference"),
    PF(section_prior_weight, "kind-transition prior weight in the DP"),
    PF(section_block_penalty,"fixed DP cost per block"),
    PI(section_min_measures, "shortest discovered repeat unit"),
    PB(chord_runs,           "progression repeat matching"),
    PI(chord_run_beats,      "max progression template beats"),
    PF(chord_sim_threshold,  "progression match threshold"),
    PB(chord_fallback,       "beat-by-beat decoder (learned models / triads)"),
    PF(chord_margin,         "(kept for the UI)"),
    PF(chord_transition,     "decoder: chord change cost"),
    PF(chord_measure_bonus,  "decoder: change cost waived on measure starts"),
    PF(chord_unseen_penalty, "decoder: penalty for triads not in the map (>=1 disables)"),
    PF(chord_prior_beats,    "learned chord model triad prior weight"),
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

// "a=1;b=2" -> set_param each
static bool set_params_str(CompleteParams* p, const char* str) {
    if (!str) return true;
    char buf[512];
    strncpy(buf, str, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    for (char* tok = strtok(buf, ";, "); tok; tok = strtok(nullptr, ";, ")) {
        char* eq = strchr(tok, '=');
        if (!eq) return false;
        *eq = 0;
        if (!set_param(p, tok, eq + 1)) { fprintf(stderr, "unknown param %s\n", tok); return false; }
    }
    return true;
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
// Track data and map sets
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
    double t_begin = 0, t_end = 0;   // first and last truth beat: the scored span
};

struct TrackData {
    std::string audio, map, name;
    float*   pcm = nullptr;
    uint64_t nf  = 0;
    uint32_t sr  = 0;
    double   duration = 0;
    Truth    tr;
};

static bool track_load(const char* audio_path, const char* map_path, TrackData* td) {
    td->audio = audio_path;
    if (map_path && map_path[0]) td->map = map_path;
    else {
        char buf[512];
        beatmap_path_for_audio(audio_path, buf, sizeof(buf));
        td->map = buf;
    }
    const char* slash = strrchr(audio_path, '/');
    td->name = slash ? slash + 1 : audio_path;
    size_t dot = td->name.rfind('.');
    if (dot != std::string::npos) td->name.resize(dot);

    BeatMap bm; SectionMap sm; LyricMap lm; MiscMap mm, cm;
    beatmap_init(&bm); sectionmap_init(&sm); lyricmap_init(&lm);
    miscmap_init(&mm); miscmap_init(&cm, "chord");
    if (!beatmap_load(&bm, &sm, &lm, &mm, &cm, td->map.c_str())) {
        fprintf(stderr, "cannot load map %s\n", td->map.c_str());
        return false;
    }
    for (int i = 0; i < bm.count; i++) td->tr.beats.push_back(bm.beats[i].time);
    td->tr.sections.assign(sm.sections, sm.sections + sm.count);
    td->tr.chords.assign(cm.entries, cm.entries + cm.count);
    beatmap_shutdown(&bm); sectionmap_shutdown(&sm); lyricmap_shutdown(&lm);
    miscmap_shutdown(&mm); miscmap_shutdown(&cm);

    if (!audio_decode_pcm(audio_path, &td->pcm, &td->nf, &td->sr)) {
        fprintf(stderr, "cannot decode %s\n", audio_path);
        return false;
    }
    td->duration = (double)td->nf / td->sr;
    td->tr.t_begin = td->tr.beats.empty() ? 0.0 : td->tr.beats.front();
    td->tr.t_end   = td->tr.beats.empty() ? td->duration : td->tr.beats.back();
    return true;
}

static void track_free(TrackData* td) {
    audio_free_pcm(td->pcm);
    td->pcm = nullptr;
}

// The mutable map a scenario works on, plus which truth beats it hid.
struct MapSet {
    BeatMap bm; SectionMap sm; LyricMap lm; MiscMap mm, cm;
    std::vector<char> hidden;                 // per truth beat
    void init(const Truth& tr) {
        beatmap_init(&bm); sectionmap_init(&sm); lyricmap_init(&lm);
        miscmap_init(&mm); miscmap_init(&cm, "chord");
        for (double t : tr.beats) beatmap_add(&bm, t);
        for (const Section& s : tr.sections) {
            int i = sectionmap_add(&sm, s.t_start, s.t_end, s.kind, s.label);
            if (i >= 0) { sm.sections[i].ts_num = s.ts_num; sm.sections[i].ts_den = s.ts_den; }
        }
        for (const MiscAnnotation& c : tr.chords) miscmap_add(&cm, c.t_start, c.t_end, c.text);
        hidden.assign(tr.beats.size(), 0);
    }
    void free() {
        beatmap_shutdown(&bm); sectionmap_shutdown(&sm); lyricmap_shutdown(&lm);
        miscmap_shutdown(&mm); miscmap_shutdown(&cm);
    }
};

// Layer masks
enum { L_BEATS = 1, L_SECTIONS = 2, L_CHORDS = 4, L_ALL = 7 };

// Hide the given layers over [t0, t1] (seconds).
static void hide_range(const Truth& tr, MapSet* ms, int layers, double t0, double t1) {
    if (t1 <= t0) return;
    if (layers & L_BEATS) {
        for (int i = ms->bm.count - 1; i >= 0; i--)
            if (ms->bm.beats[i].time >= t0 - 1e-6 && ms->bm.beats[i].time <= t1 + 1e-6)
                beatmap_remove(&ms->bm, i);
        for (size_t i = 0; i < tr.beats.size(); i++)
            if (tr.beats[i] >= t0 - 1e-6 && tr.beats[i] <= t1 + 1e-6) ms->hidden[i] = 1;
    }
    if (layers & L_SECTIONS)
        for (int i = ms->sm.count - 1; i >= 0; i--)
            if (ms->sm.sections[i].t_end > t0 && ms->sm.sections[i].t_start < t1)
                sectionmap_remove(&ms->sm, i);
    if (layers & L_CHORDS)
        for (int i = ms->cm.count - 1; i >= 0; i--)
            if (ms->cm.entries[i].t_start >= t0 - 1e-6 && ms->cm.entries[i].t_start <= t1)
                miscmap_remove(&ms->cm, i);
}

// ---------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------
struct Metrics {
    // beats
    int    bn = 0, hit50 = 0, hit90 = 0, half = 0, missed = 0, n_hidden = 0;
    double mean_ms = 0, max_ms = 0;
    int    transfers = 0, tempo_runs = 0;
    // sections
    int sec_found = 0, sec_true = 0, sec_false = 0;
    // chords
    int cb_ok = 0, cb_n = 0, ch_found = 0, ch_named = 0;
    bool has_beats = false, has_sections = false;
};

static void score_beats(const std::vector<double>& prop, const Truth& tr,
                        const std::vector<char>& hidden, Metrics* m)
{
    m->has_beats = true;
    double med = median_ibi(tr.beats);
    double err = 0;
    m->n_hidden = 0;
    for (size_t i = 0; i < hidden.size(); i++) if (hidden[i]) m->n_hidden++;
    for (double t : prop) {
        if (t > tr.t_end + 0.5 || t < tr.t_begin - 0.5) continue;
        auto it = std::lower_bound(tr.beats.begin(), tr.beats.end(), t);
        double best = 1e9;
        if (it != tr.beats.end()) best = std::min(best, fabs(*it - t));
        if (it != tr.beats.begin()) best = std::min(best, fabs(*(it - 1) - t));
        err += best; m->max_ms = std::max(m->max_ms, best * 1000.0); m->bn++;
        if (best <= 0.050) m->hit50++;
        if (best <= 0.090) m->hit90++;
        if (best > 0.3 * med && best < 0.7 * med) m->half++;
    }
    m->mean_ms = m->bn ? 1000.0 * err / m->bn : 0.0;
    for (size_t i = 0; i < tr.beats.size(); i++) {
        if (!hidden[i]) continue;
        double u = tr.beats[i];
        auto it = std::lower_bound(prop.begin(), prop.end(), u);
        double best = 1e9;
        if (it != prop.end()) best = std::min(best, fabs(*it - u));
        if (it != prop.begin()) best = std::min(best, fabs(*(it - 1) - u));
        if (best > 0.050) m->missed++;
    }
}

static void chord_token(const char* text, char* out, int n) {
    int i = 0;
    while (text[i] && text[i] != ' ' && i < n - 1) { out[i] = text[i]; i++; }
    out[i] = 0;
}

// Per-beat chord accuracy over truth beats whose chord was hidden, judged
// against every chord proposal accumulated so far (chord candidates and the
// chords riding with section candidates).
static void score_chord_beats(const Truth& tr, const std::vector<char>& chord_hidden,
                              const CompleteProposal& all, Metrics* m)
{
    for (size_t i = 0; i + 1 < tr.beats.size(); i++) {
        double t = tr.beats[i] + 1e-3;
        const MiscAnnotation* truth = nullptr;
        size_t ci = 0;
        for (; ci < tr.chords.size(); ci++)
            if (tr.chords[ci].t_start <= t && tr.chords[ci].t_end > t) { truth = &tr.chords[ci]; break; }
        if (!truth || !chord_hidden[ci]) continue;
        m->cb_n++;
        const ChordProposal* prop = nullptr;
        for (const CompleteCand& c : all.cands) {
            int first = -1, n = 0;
            if (c.kind == CAND_CHORDS) { first = c.first; n = c.n; }
            else if (c.kind == CAND_SECTION && c.chord_n > 0) { first = c.chord_first; n = c.chord_n; }
            for (int k = 0; k < n; k++) {
                const ChordProposal& cp = all.chords[first + k];
                if (cp.t0 <= t && cp.t1 > t) { prop = &cp; break; }
            }
            if (prop) break;
        }
        if (!prop) continue;
        char a[32], b[32];
        chord_token(truth->text, a, sizeof(a)); chord_token(prop->text, b, sizeof(b));
        if (!strcmp(a, b)) m->cb_ok++;
    }
}

// ---------------------------------------------------------------------------
// Scenario runner
// ---------------------------------------------------------------------------
struct RunOpts {
    const char* order   = "b,s,c";   // stages, in order; each accepts its output
    int   smooth2       = 0;
    bool  list = false, dump = false;
    bool  has_region = false;
    double r0 = 0, r1 = 0;
};

// Run the inference stages in opts.order on the (already hidden) map set,
// accepting everything between stages, and fill in the metrics.
static void run_scenario(const TrackData& td, MapSet* ms, CompleteParams p,
                         const RunOpts& opts, Metrics* m)
{
    const Truth& tr = td.tr;
    CompleteInputs in = {};
    in.beatmap = &ms->bm; in.sectionmap = &ms->sm; in.chordmap = &ms->cm;
    in.audio.pcm = td.pcm; in.audio.frame_count = td.nf; in.audio.channels = 1;
    in.audio.sample_rate = td.sr;
    in.duration = td.duration;
    in.has_region = opts.has_region; in.region_start = opts.r0; in.region_end = opts.r1;

    // Which truth sections / chords are hidden right now (before inference)
    std::vector<char> sec_hidden(tr.sections.size(), 1);
    for (size_t i = 0; i < tr.sections.size(); i++)
        for (int j = 0; j < ms->sm.count; j++)
            if (fabs(ms->sm.sections[j].t_start - tr.sections[i].t_start) < 1e-3) sec_hidden[i] = 0;
    std::vector<char> chord_hidden(tr.chords.size(), 1);
    for (size_t i = 0; i < tr.chords.size(); i++)
        for (int j = 0; j < ms->cm.count; j++)
            if (fabs(ms->cm.entries[j].t_start - tr.chords[i].t_start) < 1e-3) chord_hidden[i] = 0;

    BeatChromaCache cache;
    CompleteProposal accum;      // every chord proposal across the stages, for scoring
    CompleteProposal out;

    char order[32];
    strncpy(order, opts.order, sizeof(order) - 1); order[sizeof(order) - 1] = 0;
    for (char* stage = strtok(order, ","); stage; stage = strtok(nullptr, ",")) {
        CompleteParams sp = p;
        sp.do_beats = stage[0] == 'b';
        sp.do_sections = stage[0] == 's';
        sp.do_chords = stage[0] == 'c';
        if (!sp.do_beats && !sp.do_sections && !sp.do_chords) continue;
        complete_run(in, sp, &cache, &out);

        if (sp.do_beats) {
            for (int k = 0; k < opts.smooth2; k++)
                complete_smooth_segments(&out, &ms->bm, p.smooth2, false);
            for (const CompleteCand& c : out.cands)
                if (c.kind == CAND_BEATS) { if (!strcmp(c.source, "tempo")) m->tempo_runs++; else m->transfers++; }
            std::vector<double> prop = out.beat_times;
            std::sort(prop.begin(), prop.end());
            score_beats(prop, tr, ms->hidden, m);
            if (opts.dump)
                for (size_t i = 0; i < out.beat_times.size(); i++) {
                    double t = out.beat_times[i];
                    auto it = std::lower_bound(tr.beats.begin(), tr.beats.end(), t);
                    double d = 1e9;
                    if (it != tr.beats.end()) d = *it - t;
                    if (it != tr.beats.begin() && fabs(*(it - 1) - t) < fabs(d)) d = *(it - 1) - t;
                    printf("  %8.3f  truth %+7.3f  conf %.2f\n", t, d, out.beat_conf[i]);
                }
            for (double t : out.beat_times) beatmap_add(&ms->bm, t);
        }
        if (sp.do_sections) {
            m->has_sections = true;
            m->sec_true = 0;
            for (size_t i = 0; i < tr.sections.size(); i++) if (sec_hidden[i]) m->sec_true++;
            // The truth's sections may not cover the whole song: a proposal
            // outside the annotated section span is unknowable, not wrong.
            double ann0 = 1e18, ann1 = -1e18;
            for (const Section& ts2 : tr.sections) { ann0 = std::min(ann0, ts2.t_start); ann1 = std::max(ann1, ts2.t_end); }
            for (const CompleteCand& c : out.cands) {
                if (c.kind != CAND_SECTION) continue;
                if (c.t0 > ann1 - 0.5 || c.t1 < ann0 + 0.5) continue;
                // Discovered repeats are labelled A/B/chorus by convention and
                // cannot know the truth's names: judge them on edges alone.
                bool from_discovery = strncmp(c.source, "repeat", 6) == 0;
                bool ok = false;
                for (size_t i = 0; i < tr.sections.size(); i++)
                    if (sec_hidden[i] && fabs(tr.sections[i].t_start - c.t0) < 0.25 &&
                        fabs(tr.sections[i].t_end - c.t1) < 0.25 &&
                        (from_discovery || tr.sections[i].kind == c.sec_kind)) { ok = true; break; }
                if (ok) m->sec_found++; else m->sec_false++;
                int idx = sectionmap_add(&ms->sm, c.t0, c.t1, c.sec_kind, c.label);
                if (idx >= 0) ms->sm.sections[idx].ts_num = c.ts_num;
                for (int i = 0; i < c.chord_n; i++) {
                    const ChordProposal& cp = out.chords[c.chord_first + i];
                    miscmap_add(&ms->cm, cp.t0, cp.t1, cp.text);
                }
            }
        }
        if (sp.do_chords) {
            for (const CompleteCand& c : out.cands) {
                if (c.kind != CAND_CHORDS || c.t0 > tr.t_end + 0.5) continue;
                m->ch_found += c.n;
                for (int k = 0; k < c.n; k++) {
                    const ChordProposal& cp = out.chords[c.first + k];
                    for (const MiscAnnotation& t : tr.chords)
                        if (fabs(t.t_start - cp.t0) < 0.15) {
                            char a[32], b[32];
                            chord_token(t.text, a, sizeof(a)); chord_token(cp.text, b, sizeof(b));
                            if (!strcmp(a, b)) m->ch_named++;
                            break;
                        }
                    miscmap_add(&ms->cm, cp.t0, cp.t1, cp.text);
                }
            }
        }
        if (opts.list)
            for (const CompleteCand& c : out.cands) printf("  [%d] %.2f %s\n", c.kind, c.score, c.desc);
        // Accumulate chord proposals for the per-beat score
        int base = (int)accum.chords.size();
        accum.chords.insert(accum.chords.end(), out.chords.begin(), out.chords.end());
        for (CompleteCand c : out.cands) {
            if (c.kind == CAND_CHORDS) c.first += base;
            if (c.kind == CAND_SECTION && c.chord_n > 0) c.chord_first += base;
            accum.cands.push_back(c);
        }
    }
    bool any_chords_hidden = false;
    for (char h : chord_hidden) if (h) any_chords_hidden = true;
    if (any_chords_hidden && !tr.chords.empty())
        score_chord_beats(tr, chord_hidden, accum, m);
}

// ---------------------------------------------------------------------------
// The suite
// ---------------------------------------------------------------------------
// Spans are fractions of the annotated range.  Each row hides layers, then
// re-infers in the given order (each stage accepts its output before the
// next runs).  "keepsec0" hides everything outside the first section.
struct Scen {
    const char* name;
    int    layers;
    double f0, f1;
    bool   keepsec0;
    const char* order;
    int    smooth2;
    const char* overrides;
};
static const Scen SUITE[] = {
    { "beats/tail-50",    L_BEATS, 0.50, 1.00, false, "b",   1, nullptr },
    { "beats/mid-25",     L_BEATS, 0.375, 0.625, false, "b", 1, nullptr },
    { "beats/chroma-alt", L_BEATS, 0.50, 1.00, false, "b",   1, "fill_algo_idx=0" },
    { "sect/discover",    L_SECTIONS, 0.0, 1.0, false, "s",  0, nullptr },
    { "sect/tail-50",     L_SECTIONS | L_CHORDS, 0.50, 1.0, false, "s,c", 0, nullptr },
    { "chords/tail-50",   L_CHORDS, 0.50, 1.0, false, "c",   0, nullptr },
    { "chords/decoder",   L_CHORDS | L_SECTIONS, 0.50, 1.0, false, "c", 0, "chord_runs=0;section_discover=0" },
    { "all/keepsec0",     L_ALL, 0, 0, true,  "b,s,c",       1, nullptr },
    { "all/tail-60",      L_ALL, 0.40, 1.0, false, "b,s,c",  1, nullptr },
    { "all/chords-first", L_ALL, 0.40, 1.0, false, "b,c,s",  1, nullptr },
};
static const int N_SUITE = (int)(sizeof(SUITE) / sizeof(SUITE[0]));

static void metrics_print(const char* track, const char* scen, const Metrics& m, bool tsv) {
    if (tsv) {
        printf("%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%.1f\t%.1f\t%d\t%d\t%d\t%d\t%d\n",
               track, scen, m.n_hidden, m.bn, m.hit50, m.hit90, m.half, m.missed, m.mean_ms, m.max_ms,
               m.sec_found, m.sec_true, m.sec_false, m.cb_ok, m.cb_n);
        return;
    }
    printf("  %-18s", scen);
    if (m.has_beats && m.bn > 0)
        printf("  beats %3d%%/%3d%% of %-4d half %-3d mean %5.1fms",
               100 * m.hit50 / m.bn, 100 * m.hit90 / m.bn, m.bn, m.half, m.mean_ms);
    else if (m.has_beats)
        printf("  beats: none proposed (%d hidden)", m.n_hidden);
    if (m.has_sections)
        printf("  sect %d/%d (+%d wrong)", m.sec_found, m.sec_true, m.sec_false);
    if (m.cb_n > 0)
        printf("  chords %3d%% of %d beats", 100 * m.cb_ok / m.cb_n, m.cb_n);
    printf("\n");
}

// Leave-one-out: for every section whose kind appears >= 3 times, hide just
// that instance (sections + chords over its span; beats stay) and ask the
// section stage to bring it back.  The easy case: the section is elsewhere in
// the track, twice, nearly identically.  One metrics row aggregates all runs.
static void run_loo(const TrackData& td, const CompleteParams& base, bool tsv, bool verbose)
{
    const Truth& tr = td.tr;
    Metrics agg;
    agg.has_sections = true;
    for (size_t i = 0; i < tr.sections.size(); i++) {
        int same = 0;
        for (size_t j = 0; j < tr.sections.size(); j++)
            if (tr.sections[j].kind == tr.sections[i].kind) same++;
        if (same < 3) continue;

        MapSet ms; ms.init(tr);
        hide_range(tr, &ms, L_SECTIONS | L_CHORDS,
                   tr.sections[i].t_start + 1e-3, tr.sections[i].t_end - 1e-3);
        CompleteParams p = base;
        RunOpts opts; opts.order = "s";
        Metrics m;
        shape_analysis_clear(shape_track());
        run_scenario(td, &ms, p, opts, &m);
        agg.sec_true += m.sec_true;
        agg.sec_found += m.sec_found;
        agg.sec_false += m.sec_false;
        agg.cb_ok += m.cb_ok; agg.cb_n += m.cb_n;
        if (verbose && m.sec_found < m.sec_true) {
            char name[64];
            if (tr.sections[i].label[0]) snprintf(name, sizeof(name), "%s %s", SECTION_KIND_NAMES[tr.sections[i].kind], tr.sections[i].label);
            else snprintf(name, sizeof(name), "%s", SECTION_KIND_NAMES[tr.sections[i].kind]);
            printf("    MISS %-16s %7.2f-%-7.2f (+%d wrong)", name, tr.sections[i].t_start, tr.sections[i].t_end, m.sec_false);
            // nearest proposal (accepted into ms.sm beyond the truth count)
            double best = 1e9; double b0 = 0, b1 = 0; const char* bk = "";
            for (int k = 0; k < ms.sm.count; k++) {
                const Section& c = ms.sm.sections[k];
                bool was_truth = false;
                for (size_t q = 0; q < tr.sections.size(); q++)
                    if (q != i && fabs(tr.sections[q].t_start - c.t_start) < 1e-3) was_truth = true;
                if (was_truth) continue;
                double d = fabs(c.t_start - tr.sections[i].t_start) + fabs(c.t_end - tr.sections[i].t_end);
                if (d < best) { best = d; b0 = c.t_start; b1 = c.t_end; bk = SECTION_KIND_NAMES[c.kind]; }
            }
            if (best < 1e9) printf("  nearest %s %7.2f-%-7.2f (edge err %.2fs)\n", bk, b0, b1, best);
            else printf("  nothing proposed\n");
        }
        ms.free();
    }
    metrics_print(td.name.c_str(), "sect/loo", agg, tsv);
}

static const char* TSV_HEADER =
    "track\tscenario\thidden\tn\thit50\thit90\thalf\tmissed\tmean_ms\tmax_ms\t"
    "sec_found\tsec_true\tsec_false\tcb_ok\tcb_n\n";

static void run_suite(const TrackData& td, const CompleteParams& base, const char* only,
                      bool tsv)
{
    if (!tsv)
        printf("== %s  (%d beats, %d sections, %d chords)\n", td.name.c_str(),
               (int)td.tr.beats.size(), (int)td.tr.sections.size(), (int)td.tr.chords.size());
    for (int s = 0; s < N_SUITE; s++) {
        const Scen& sc = SUITE[s];
        if (only && strncmp(sc.name, only, strlen(only)) != 0) continue;
        if ((sc.layers & L_CHORDS) && !(sc.layers & L_BEATS) && td.tr.chords.empty()) continue;
        if (sc.keepsec0 && td.tr.sections.empty()) continue;

        MapSet ms; ms.init(td.tr);
        if (sc.keepsec0) {
            double s0 = td.tr.sections[0].t_start, s1 = td.tr.sections[0].t_end;
            hide_range(td.tr, &ms, L_ALL, 0.0, s0 - 1e-3);
            hide_range(td.tr, &ms, L_ALL, s1 + 1e-3, td.duration);
        } else {
            double t0 = td.tr.t_begin + sc.f0 * (td.tr.t_end - td.tr.t_begin);
            double t1 = td.tr.t_begin + sc.f1 * (td.tr.t_end - td.tr.t_begin);
            if (sc.f0 <= 0.0) t0 = 0.0;
            if (sc.f1 >= 1.0) t1 = td.duration;
            hide_range(td.tr, &ms, sc.layers, t0, t1);
        }
        CompleteParams p = base;
        set_params_str(&p, sc.overrides);
        RunOpts opts;
        opts.order = sc.order;
        opts.smooth2 = sc.smooth2;
        Metrics m;
        shape_analysis_clear(shape_track());   // no cross-scenario reuse
        run_scenario(td, &ms, p, opts, &m);
        metrics_print(td.name.c_str(), sc.name, m, tsv);
        ms.free();
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
static void usage() {
    fprintf(stderr,
        "usage: bmbench <audio-or-dir> [--map FILE] <suite|complete|detect|shapes|params> [options]\n"
        "  suite:    [--only PREFIX] [--set NAME=VALUE ...] [--tsv [--header]]\n"
        "            <audio-or-dir> may be a directory of track/.txt pairs (e.g. bench/tracks)\n"
        "  complete: --keep T0-T1 | --drop T0-T1 | --hide b|s|c|bs|bc|... T0-T1 | --keep-section N\n"
        "            --no-sections --no-chords --edge-ms MS --region T0-T1\n"
        "            --order b,s,c  --algo NAME|IDX  --set NAME=VALUE  --smooth2 N\n"
        "            --tsv [--header]  --list  --dump  --debug\n"
        "  detect:   --region T0-T1 [--set det_*=...]\n"
        "  shapes:   [--set shape.*=...]\n");
}

static bool is_dir(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    const char* target = argv[1];
    std::string map_path;
    int ai = 2;
    if (!strcmp(argv[ai], "--map") && ai + 1 < argc) { map_path = argv[ai + 1]; ai += 2; }
    if (ai >= argc) { usage(); return 2; }
    std::string cmd = argv[ai++];

    CompleteParams p; complete_params_defaults(&p);
    if (cmd == "params") { print_params(p); return 0; }

    struct Hide { int layers; double a, b; int sec; };  // sec >= 0: keep-section; -2: keep-range
    std::vector<Hide> hides;
    bool first_keep = true;
    int smooth2 = 0; bool tsv = false, header = false, list = false, dump = false;
    bool no_sections = false, no_chords = false;
    const char* order = "b,s,c";
    const char* only = nullptr;
    double edge_ms = 0.0;
    double reg0 = 0, reg1 = 0; bool have_reg = false;

    auto layer_mask = [](const char* s) {
        int m = 0;
        for (; *s; s++) { if (*s == 'b') m |= L_BEATS; if (*s == 's') m |= L_SECTIONS; if (*s == 'c') m |= L_CHORDS; }
        return m;
    };

    for (; ai < argc; ai++) {
        const char* a = argv[ai];
        const char* v = (ai + 1 < argc) ? argv[ai + 1] : nullptr;
        if (!strcmp(a, "--keep") && v)      { Hide h = { L_ALL, 0, 0, -2 }; if (!parse_range(v, &h.a, &h.b)) return 2; if (!first_keep) h.sec = -1; first_keep = false; hides.push_back(h); ai++; }
        else if (!strcmp(a, "--drop") && v) { Hide h = { L_ALL, 0, 0, -1 }; if (!parse_range(v, &h.a, &h.b)) return 2; hides.push_back(h); ai++; }
        else if (!strcmp(a, "--hide") && v && ai + 2 < argc) {
            Hide h = { layer_mask(v), 0, 0, -1 };
            if (!h.layers || !parse_range(argv[ai + 2], &h.a, &h.b)) { fprintf(stderr, "--hide takes layers (b/s/c) and a range\n"); return 2; }
            hides.push_back(h); ai += 2;
        }
        else if (!strcmp(a, "--hide-chords") && v) { Hide h = { L_CHORDS, 0, 0, -1 }; if (!parse_range(v, &h.a, &h.b)) return 2; hides.push_back(h); ai++; }
        else if (!strcmp(a, "--keep-section") && v) { Hide h = { L_ALL, 0, 0, atoi(v) }; hides.push_back(h); ai++; }
        else if (!strcmp(a, "--no-sections")) no_sections = true;
        else if (!strcmp(a, "--no-chords"))   no_chords = true;
        else if (!strcmp(a, "--edge-ms") && v) { edge_ms = atof(v); ai++; }
        else if (!strcmp(a, "--region") && v) { if (!parse_range(v, &reg0, &reg1)) return 2; have_reg = true; ai++; }
        else if (!strcmp(a, "--order") && v)  { order = v; ai++; }
        else if (!strcmp(a, "--only") && v)   { only = v; ai++; }
        else if (!strcmp(a, "--algo") && v) {
            bool ok = false;
            for (int i = 0; i < complete_fill_algo_count(); i++)
                if (!strcasecmp(complete_fill_algo_name(i), v) || (isdigit((unsigned char)v[0]) && atoi(v) == i)) { p.fill_algo_idx = i; ok = true; }
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
        else if (!strcmp(a, "--tol") && v)     { ai++; /* fixed 50/90 ms now */ }
        else if (!strcmp(a, "--tsv"))     tsv = true;
        else if (!strcmp(a, "--header"))  header = true;
        else if (!strcmp(a, "--list"))    list = true;
        else if (!strcmp(a, "--dump"))    dump = true;
        else if (!strcmp(a, "--stages")) { /* legacy: the default order already runs all stages */ }
        else if (!strcmp(a, "--debug"))   setenv("COMPLETE_DEBUG", "1", 1);
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
    }

    // Targets: one track, or every audio/.txt pair in a directory
    std::vector<std::string> targets;
    if (is_dir(target)) {
        DIR* d = opendir(target);
        struct dirent* de;
        while (d && (de = readdir(d)) != nullptr) {
            const char* dot = strrchr(de->d_name, '.');
            if (!dot || (strcasecmp(dot, ".mp3") && strcasecmp(dot, ".m4a") &&
                         strcasecmp(dot, ".wav") && strcasecmp(dot, ".flac"))) continue;
            std::string full = std::string(target) + "/" + de->d_name;
            std::string txt = full.substr(0, full.rfind('.')) + ".txt";
            struct stat st;
            if (stat(txt.c_str(), &st) == 0) targets.push_back(full);
        }
        if (d) closedir(d);
        std::sort(targets.begin(), targets.end());
        if (targets.empty()) { fprintf(stderr, "no track/.txt pairs in %s\n", target); return 1; }
        if (cmd != "suite") { fprintf(stderr, "a directory target needs the suite command\n"); return 2; }
    } else {
        targets.push_back(target);
    }

    if (tsv && header && (cmd == "suite" || cmd == "complete")) printf("%s", TSV_HEADER);

    for (const std::string& t : targets) {
        TrackData td;
        if (!track_load(t.c_str(), map_path.c_str(), &td)) return 1;

        if (cmd == "shapes") {
            ShapeAnalysis* sa = shape_track();
            MapSet ms; ms.init(td.tr);
            AudioPcm au = { td.pcm, td.nf, 1, td.sr };
            shape_analysis_ensure(sa, au, &ms.bm, td.duration, p.shape, p.beat_algo_idx);
            printf("%d onsets, window %.0f ms, vocabulary %s\n", (int)sa->onset_t.size(), sa->win * 1000.0,
                   sa->vocab.valid ? "ok" : "none");
            for (int c = 0; sa->vocab.valid && c < sa->vocab.k; c++) {
                int on = 0, half = 0, other = 0, n = 0;
                for (size_t i = 0; i < sa->onset_t.size(); i++) {
                    if (sa->onset_shape[i] != c) continue;
                    n++;
                    double t2 = sa->onset_t[i];
                    auto it = std::lower_bound(td.tr.beats.begin(), td.tr.beats.end(), t2);
                    if (it == td.tr.beats.begin() || it == td.tr.beats.end()) continue;
                    double ph = (t2 - *(it - 1)) / (*it - *(it - 1));
                    if (ph < 0.1 || ph > 0.9) on++; else if (ph > 0.4 && ph < 0.6) half++; else other++;
                }
                printf("shape %d: %4d hits  %.0f dB  on-beat %4d  half-beat %4d  other %4d\n",
                       c, n, sa->vocab.energy[c], on, half, other);
            }
            ms.free(); track_free(&td);
            continue;
        }
        if (cmd == "detect") {
            if (!have_reg) { fprintf(stderr, "detect needs --region\n"); return 2; }
            static AutoBeatList ab; autobeat_init(&ab);
            BeatAlgoParams bp = {};
            bp.min_bpm = p.det_min_bpm; bp.max_bpm = p.det_max_bpm;
            bp.onset_threshold = p.det_threshold; bp.dp_tightness = p.det_tightness;
            BEAT_ALGOS[p.beat_algo_idx].fn(td.pcm, td.nf, 1, td.sr, reg0, reg1, &bp, &ab);
            std::vector<double> prop(ab.beat_times, ab.beat_times + ab.beat_count);
            std::sort(prop.begin(), prop.end());
            std::vector<char> hidden(td.tr.beats.size(), 0);
            for (size_t i = 0; i < td.tr.beats.size(); i++)
                hidden[i] = td.tr.beats[i] >= reg0 && td.tr.beats[i] <= reg1;
            Metrics m;
            score_beats(prop, td.tr, hidden, &m);
            metrics_print(td.name.c_str(), "detect", m, tsv);
            track_free(&td);
            continue;
        }
        if (cmd == "suite") {
            if (only && !strcmp(only, "sect/loo")) run_loo(td, p, tsv, list || !tsv);
            else run_suite(td, p, only, tsv);
            track_free(&td);
            continue;
        }
        if (cmd != "complete") { usage(); return 2; }

        MapSet ms; ms.init(td.tr);
        if (edge_ms != 0.0) {
            for (int i = 0; i < ms.sm.count; i++) { ms.sm.sections[i].t_start += edge_ms / 1000.0; ms.sm.sections[i].t_end += edge_ms / 1000.0; }
            for (int i = 0; i < ms.cm.count; i++) { ms.cm.entries[i].t_start += edge_ms / 1000.0; ms.cm.entries[i].t_end += edge_ms / 1000.0; }
        }
        for (const Hide& h : hides) {
            if (h.sec >= 0) {
                if (h.sec >= (int)td.tr.sections.size()) { fprintf(stderr, "no section %d\n", h.sec); return 2; }
                double s0 = td.tr.sections[h.sec].t_start, s1 = td.tr.sections[h.sec].t_end;
                hide_range(td.tr, &ms, L_ALL, 0.0, s0 - 1e-3);
                hide_range(td.tr, &ms, L_ALL, s1 + 1e-3, td.duration);
            } else if (h.sec == -2) {   // keep: hide everything outside the range
                hide_range(td.tr, &ms, L_ALL, 0.0, h.a - 1e-3);
                hide_range(td.tr, &ms, L_ALL, h.b + 1e-3, td.duration);
            } else {
                hide_range(td.tr, &ms, h.layers, h.a, h.b);
            }
        }
        if (no_sections) hide_range(td.tr, &ms, L_SECTIONS, 0.0, td.duration);
        if (no_chords)   hide_range(td.tr, &ms, L_CHORDS, 0.0, td.duration);

        RunOpts opts;
        opts.order = order; opts.smooth2 = smooth2;
        opts.list = list; opts.dump = dump;
        opts.has_region = have_reg; opts.r0 = reg0; opts.r1 = reg1;
        Metrics m;
        run_scenario(td, &ms, p, opts, &m);
        metrics_print(td.name.c_str(), "complete", m, tsv);
        ms.free();
        track_free(&td);
    }
    return 0;
}
