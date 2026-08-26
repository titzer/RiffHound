#include "complete_algo.h"
#include "beat_algo.h"
#include "onset_shape.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

// ===========================================================================
// Parameters
// ===========================================================================

void complete_params_defaults(CompleteParams* p) {
    p->do_beats = p->do_sections = p->do_chords = true;

    p->gap_factor         = 1.8f;
    p->template_beats     = 16;
    p->min_template_beats = 4;
    p->max_warp           = 0.08f;
    p->warp_steps         = 7;
    p->warp_weight        = 2.0f;
    p->jitter_beats       = 0.0f;      // sub-beat slack lets templates land half a beat off; off by default
    p->lookahead_beats    = 4;         // a template may instead start a few whole beats ahead
    p->lookahead_penalty  = 0.10f;     // ... if it beats the immediate option by this much per beat
    p->beat_sim_threshold = 0.55f;
    p->beat_algo_idx      = 0;
    p->fill_algo_idx      = 2;         // anchor + region growing
    p->rhythm_weight      = 1.0f;
    p->miss_penalty       = 0.15f;
    p->extra_penalty      = 0.15f;
    shape_params_defaults(&p->shape);
    p->det_min_bpm        = 60.0f;
    p->det_max_bpm        = 200.0f;
    p->det_threshold      = 1.5f;
    p->det_tightness      = 400.0f;
    p->onset_weight       = 0.5f;
    p->refit              = true;
    p->grid_follow        = true;
    p->onset_window       = 0.2f;
    smooth_params_defaults(&p->smooth);
    p->smooth.strength    = 0.2f;      // light: transferred rubato should survive
    p->smooth.iterations  = 5;
    p->smooth.max_shift   = 0.030f;
    p->smooth.use_onsets  = true;
    p->smooth.onset_weight = 0.35f;
    p->smooth.onset_window = 0.2f;
    smooth_params_defaults(&p->smooth2);
    p->smooth2.use_onsets   = true;

    p->section_granularity   = 0;
    p->section_sim_threshold = 0.75f;
    p->section_max_overlap   = 0.10f;
    p->section_rhythm_weight = 0.4f;
    p->section_discover      = true;
    p->section_partition     = true;
    p->section_block_penalty = 2.0f;
    p->section_prior_weight  = 0.0f;   // sparse bigrams mislead more than they help; off by default
    p->section_min_measures  = 4;

    p->chord_runs          = true;
    p->chord_run_beats     = 64;
    p->chord_sim_threshold = 0.70f;
    p->chord_fallback      = true;
    p->chord_margin        = 0.12f;
    p->chord_transition    = 0.10f;
    p->chord_measure_bonus = 0.5f;
    p->chord_unseen_penalty = 0.10f;
    p->chord_prior_beats   = 4.0f;
    p->chord_learn_rate    = true;

    beat_chroma_params_defaults(&p->chroma);
}

// ===========================================================================
// Small helpers
// ===========================================================================

static void fmt_time(char* out, int n, double t) {
    int m = (int)(t / 60.0);
    snprintf(out, n, "%d:%05.2f", m, t - m * 60.0);
}

static double median_ibi(const BeatMap* bm) {
    if (bm->count < 2) return 0.5;
    std::vector<double> d;
    d.reserve(bm->count);
    for (int i = 1; i < bm->count; i++) d.push_back(bm->beats[i].time - bm->beats[i - 1].time);
    std::sort(d.begin(), d.end());
    return d[d.size() / 2];
}

// Index of the first beat at or after t.
static int first_beat_at(const BeatMap* bm, double t) {
    int lo = 0, hi = bm->count;
    while (lo < hi) { int m = (lo + hi) / 2; if (bm->beats[m].time < t) lo = m + 1; else hi = m; }
    return lo;
}

// Index of the beat nearest to t.  Section and chord edges are meant to sit
// on beats; taking the first beat *after* an edge that lands a few ms past
// its beat would shift everything built on it one beat late.
static int nearest_beat(const BeatMap* bm, double t) {
    int i = first_beat_at(bm, t);
    if (i >= bm->count) return bm->count - 1;
    if (i > 0 && t - bm->beats[i - 1].time < bm->beats[i].time - t) return i - 1;
    return i;
}

bool complete_cand_in_range(const CompleteCand& c, double r0, double r1) {
    if (r1 < r0) std::swap(r0, r1);
    double len = c.t1 - c.t0;
    double o0 = std::max(c.t0, r0), o1 = std::min(c.t1, r1);
    double ov = o1 - o0;
    if (ov <= 0.0) return false;
    if (len <= 1e-9) return true;
    if (ov >= 0.5 * len) return true;
    return (r0 >= c.t0 && r1 <= c.t1);      // region inside the candidate
}

static double overlap_len(double a0, double a1, double b0, double b1) {
    double o = std::min(a1, b1) - std::max(a0, b0);
    return o > 0.0 ? o : 0.0;
}

// ===========================================================================
// Templates: stretches of mapped beats with their chroma
// ===========================================================================

struct Template {
    char  name[48];
    int   ts_num;
    int   b0, b1;                       // beat index range in the map (inclusive)
    std::vector<double> off;            // beat times relative to the first (off[0] = 0)
    std::vector<const float*> chroma;   // per interval, from the cache (size off.size()-1)
    // Rhythm pattern: per interval and sub-beat slot, [presence, soft shape k...]
    int   slots = 0;
    std::vector<float> pattern;
};

// Fill t.pattern from the classified onsets inside the template's beats.
static void build_pattern(Template* t, const BeatMap* bm, const ShapeAnalysis* sa, int slots) {
    t->pattern.clear();
    t->slots = slots;
    if (!sa || !sa->vocab.valid || slots < 1) return;
    int K = sa->vocab.k, stride = 1 + K;
    int n_int = (int)t->off.size() - 1;
    t->pattern.assign((size_t)n_int * slots * stride, 0.0f);
    float emax = -120.0f, emin = 0.0f;
    int i0 = shape_onset_at(*sa, bm->beats[t->b0].time), i1 = shape_onset_at(*sa, bm->beats[t->b1].time);
    for (int i = i0; i < i1; i++) { emax = std::max(emax, sa->onset_energy[i]); emin = std::min(emin, sa->onset_energy[i]); }
    for (int k = 0; k < n_int; k++) {
        double a = bm->beats[t->b0 + k].time, b = bm->beats[t->b0 + k + 1].time;
        int ia = shape_onset_at(*sa, a), ib = shape_onset_at(*sa, b);
        for (int i = ia; i < ib; i++) {
            int s = (int)((sa->onset_t[i] - a) / (b - a) * slots);
            if (s < 0) s = 0; if (s >= slots) s = slots - 1;
            // Loudness relative to the template's loudest hit, floored so quiet hats count
            float pres = 0.4f + 0.6f * (sa->onset_energy[i] - (emax - 30.0f)) / 30.0f;
            if (pres < 0.2f) pres = 0.2f; if (pres > 1.0f) pres = 1.0f;
            float* slot = &t->pattern[((size_t)k * slots + s) * stride];
            if (pres > slot[0]) {
                slot[0] = pres;
                for (int c = 0; c < K; c++) slot[1 + c] = sa->onset_soft[(size_t)i * K + c];
            }
        }
    }
}

// Mapped spans: runs of beats not broken by a gap.
struct Span { int b0, b1; };

static void find_spans(const BeatMap* bm, double gap_thresh, std::vector<Span>* out) {
    out->clear();
    if (bm->count == 0) return;
    int s = 0;
    for (int i = 1; i < bm->count; i++) {
        if (bm->beats[i].time - bm->beats[i - 1].time > gap_thresh) {
            out->push_back({ s, i - 1 });
            s = i;
        }
    }
    out->push_back({ s, bm->count - 1 });
}

static void build_templates(const CompleteInputs& in, const CompleteParams& p,
                            const BeatChromaCache* cache, const ShapeAnalysis* shapes,
                            double gap_thresh, std::vector<Template>* out)
{
    out->clear();
    const BeatMap* bm = in.beatmap;
    if (bm->count < 2) return;

    auto make = [&](const char* name, int ts, int b0, int b1) {
        if (b1 - b0 + 1 < p.min_template_beats) return;
        // Every interval inside must be mapped (no gap)
        for (int i = b0 + 1; i <= b1; i++)
            if (bm->beats[i].time - bm->beats[i - 1].time > gap_thresh) return;
        Template t;
        strncpy(t.name, name, sizeof(t.name) - 1); t.name[sizeof(t.name) - 1] = 0;
        t.ts_num = ts > 0 ? ts : 4;
        t.b0 = b0; t.b1 = b1;
        for (int i = b0; i <= b1; i++) t.off.push_back(bm->beats[i].time - bm->beats[b0].time);
        for (int i = b0; i < b1; i++)
            t.chroma.push_back(i < (int)cache->entries.size() ? cache->entries[i].v : nullptr);
        build_pattern(&t, bm, shapes, p.shape.slots_per_beat);
        out->push_back(t);
    };

    // Sections that are fully mapped
    std::vector<char> covered(bm->count, 0);
    for (int s = 0; s < in.sectionmap->count; s++) {
        const Section& sec = in.sectionmap->sections[s];
        int b0 = nearest_beat(bm, sec.t_start);
        int b1 = nearest_beat(bm, sec.t_end);
        if (b1 - b0 + 1 < p.min_template_beats) continue;
        char name[48];
        if (sec.label[0]) snprintf(name, sizeof(name), "%s %s", SECTION_KIND_NAMES[sec.kind], sec.label);
        else              snprintf(name, sizeof(name), "%s", SECTION_KIND_NAMES[sec.kind]);
        size_t before = out->size();
        make(name, sec.ts_num, b0, b1);
        if (out->size() > before)
            for (int i = b0; i <= b1; i++) covered[i] = 1;
    }

    // Unsectioned mapped beats, chunked
    std::vector<Span> spans;
    find_spans(bm, gap_thresh, &spans);
    int chunk = p.template_beats < p.min_template_beats ? p.min_template_beats : p.template_beats;
    for (const Span& sp : spans) {
        int i = sp.b0;
        while (i <= sp.b1) {
            // Skip covered beats
            while (i <= sp.b1 && covered[i]) i++;
            if (i > sp.b1) break;
            int j = i;
            while (j <= sp.b1 && !covered[j] && j - i < chunk) j++;
            int b1 = j - 1;
            // Merge a short trailing remainder into this chunk
            if (b1 < sp.b1 && !covered[b1 + 1]) {
                int k = b1 + 1;
                while (k <= sp.b1 && !covered[k]) k++;
                if (k - 1 - b1 < p.min_template_beats) b1 = k - 1;
            }
            char name[48];
            snprintf(name, sizeof(name), "beats %d-%d", i, b1);
            make(name, 4, i, b1);
            i = b1 + 1;
        }
    }
}

// ===========================================================================
// Gaps
// ===========================================================================

struct Gap {
    double t0, t1;          // audio range to fill (exclusive of anchors)
    bool   has_l, has_r;    // anchored by a mapped beat at t0 / t1
    double period_l, period_r;   // local beat period just outside each edge (0 = unknown)
};

static double local_period(const BeatMap* bm, int idx, int dir, double fallback) {
    // Mean of up to 8 intervals walking away from idx in direction dir.
    double sum = 0; int n = 0;
    int i = idx;
    for (int k = 0; k < 8; k++) {
        int j = i + dir;
        if (j < 0 || j >= bm->count) break;
        double d = fabs(bm->beats[j].time - bm->beats[i].time);
        if (d > fallback * 1.8 || d < fallback * 0.4) break;   // hit a gap or a glitch
        sum += d; n++; i = j;
    }
    return n ? sum / n : fallback;
}

static void find_gaps(const CompleteInputs& in, double med, double gap_thresh,
                      std::vector<Gap>* out)
{
    out->clear();
    const BeatMap* bm = in.beatmap;
    auto push = [&](double t0, double t1, bool hl, bool hr, int il, int ir) {
        Gap g;
        g.t0 = t0; g.t1 = t1; g.has_l = hl; g.has_r = hr;
        g.period_l = hl ? local_period(bm, il, -1, med) : 0.0;
        g.period_r = hr ? local_period(bm, ir, +1, med) : 0.0;
        if (!hl && !hr) g.period_l = g.period_r = med;
        if (!hl) g.period_l = g.period_r;
        if (!hr) g.period_r = g.period_l;
        out->push_back(g);
    };
    if (bm->count == 0) {
        push(0.0, in.duration, false, false, -1, -1);
    } else {
        if (bm->beats[0].time > 1.5 * med)
            push(0.0, bm->beats[0].time, false, true, -1, 0);
        for (int i = 1; i < bm->count; i++)
            if (bm->beats[i].time - bm->beats[i - 1].time > gap_thresh)
                push(bm->beats[i - 1].time, bm->beats[i].time, true, true, i - 1, i);
        if (in.duration - bm->beats[bm->count - 1].time > 1.5 * med)
            push(bm->beats[bm->count - 1].time, in.duration, true, false, bm->count - 1, -1);
    }

    // Region: keep gaps that intersect it, clipped -- but a border within one
    // beat of a real anchor keeps the anchor, so the fill still lands on it.
    if (in.has_region) {
        double r0 = std::min(in.region_start, in.region_end);
        double r1 = std::max(in.region_start, in.region_end);
        std::vector<Gap> kept;
        for (Gap g : *out) {
            if (g.t1 <= r0 || g.t0 >= r1) continue;
            if (r0 > g.t0 + g.period_l) { g.t0 = r0; g.has_l = false; }
            if (r1 < g.t1 - g.period_r) { g.t1 = r1; g.has_r = false; }
            if (g.t1 - g.t0 > 0.5 * med) kept.push_back(g);
        }
        out->swap(kept);
    }
}

// ===========================================================================
// Chroma frames over an unmapped stretch (no beats to sync to yet)
// ===========================================================================

struct FrameGrid {
    double t0, hop, win;
    std::vector<float> v;   // 12 per frame
    int count() const { return (int)(v.size() / 12); }
    const float* at(double t) const {
        int i = (int)floor((t - t0) / hop + 0.5);
        if (i < 0) i = 0;
        if (i >= count()) i = count() - 1;
        return &v[(size_t)i * 12];
    }
};

static void build_frames(const AudioPcm& a, const BeatChromaParams& cp,
                         double t0, double t1, double period, FrameGrid* g)
{
    g->t0  = t0;
    g->win = period;
    g->hop = period * 0.25;
    g->v.clear();
    if (t1 <= t0 || g->hop <= 0.0) return;
    int n = (int)((t1 - t0) / g->hop) + 1;
    g->v.resize((size_t)n * 12);
    for (int i = 0; i < n; i++) {
        double s = t0 + i * g->hop;
        beat_chroma_interval(a, s, s + g->win, cp, &g->v[(size_t)i * 12]);
    }
}

// ===========================================================================
// Gap-fill strategies
// ===========================================================================
//
// Everything a strategy needs to judge "does this template, placed here with
// this stretch, fit the audio?" and "which onset should this beat land on?".
// The greedy fill loop, onset snapping, smoothing and candidate emission are
// shared; a strategy only supplies the scorer and the snap policy.

struct FillCtx {
    const CompleteInputs*        in;
    const CompleteParams*        p;
    const std::vector<Template>* tm;
    const ShapeAnalysis*         shapes;   // may be invalid (no vocabulary)
    Gap                          gap;
    double                       period;
    FrameGrid                    fg;       // chroma frames across the gap
    std::vector<double>          onsets;   // detector onsets across the gap (sorted)
    std::vector<double>          det;      // detector's own beat grid (seeded, tempo-following)
    int                          so0, so1; // shape-onset index range covering the gap
    std::vector<float>           beat_prior; // on-beat shape prior (unit length), for ranking
};

// The beat one step after `pos`: the detector's next beat when it has one in
// a plausible range, else a plain tempo step.  Keeps tempo fills and lookahead
// positions on a grid that follows the audio rather than one that drifts.
static double next_grid_beat(const FillCtx& ctx, double pos, double per) {
    auto it = std::upper_bound(ctx.det.begin(), ctx.det.end(), pos + 0.6 * per);
    if (it != ctx.det.end() && *it <= pos + 1.4 * per) return *it;
    return pos + per;
}

// Mirror image, for growing a region leftward.
static double prev_grid_beat(const FillCtx& ctx, double pos, double per) {
    auto it = std::lower_bound(ctx.det.begin(), ctx.det.end(), pos - 0.6 * per);
    if (it != ctx.det.begin() && *(it - 1) >= pos - 1.4 * per) return *(it - 1);
    return pos - per;
}

struct PlaceScore { float sim, rhythm, score; };

struct BeatFillAlgo {
    const char* name;
    const char* tip;
    void (*prepare)(FillCtx* ctx);
    // Score template t placed with its first beat at `start`, stretched by r,
    // over its first `fitted` intervals.  False when it cannot be evaluated.
    bool (*place)(const FillCtx& ctx, const Template& t, double start, double r,
                  int fitted, PlaceScore* out);
    // Onset to pull beat k of template t (nullptr for a tempo-fill beat) toward;
    // *found false when none qualifies.
    double (*snap)(const FillCtx& ctx, const Template* t, int k, double at, bool* found);
};

static double nearest_onset(const std::vector<double>& on, double t, double win, bool* found) {
    *found = false;
    if (on.empty()) return t;
    auto it = std::lower_bound(on.begin(), on.end(), t);
    double best = t, bd = win + 1.0;
    if (it != on.end())   { double d = fabs(*it - t);       if (d < bd) { bd = d; best = *it; } }
    if (it != on.begin()) { double d = fabs(*(it - 1) - t); if (d < bd) { bd = d; best = *(it - 1); } }
    if (bd <= win) { *found = true; return best; }
    return t;
}

// Mean chroma cosine of template t's intervals against the frame grid.
static float chroma_fit(const FillCtx& ctx, const Template& t, double start, double r, int fitted) {
    if (ctx.fg.count() == 0) return 0.0f;
    float sim = 0; int cnt = 0;
    for (int k = 0; k < fitted; k++) {
        if (!t.chroma[k]) continue;
        sim += chroma_cosine(t.chroma[k], ctx.fg.at(start + r * t.off[k]));
        cnt++;
    }
    return cnt ? sim / cnt : 0.0f;
}

// --- 1. Chroma transfer -----------------------------------------------------

static void chroma_prepare(FillCtx* ctx) { (void)ctx; }

static bool chroma_place(const FillCtx& ctx, const Template& t, double start, double r,
                         int fitted, PlaceScore* out)
{
    if (ctx.fg.count() == 0) return false;
    int n_int = (int)t.off.size() - 1;
    float sim = chroma_fit(ctx, t, start, r, fitted);
    float cover = (float)fitted / (float)n_int;
    out->sim = sim; out->rhythm = 0.0f;
    out->score = sim * (0.7f + 0.3f * cover) - ctx.p->warp_weight * (float)fabs(r - 1.0);
    return true;
}

static double chroma_snap(const FillCtx& ctx, const Template* t, int k, double at, bool* found) {
    (void)t; (void)k;
    return nearest_onset(ctx.onsets, at, ctx.p->onset_window * ctx.period, found);
}

// --- 2. Rhythm-shape transfer -----------------------------------------------
//
// The template carries, per beat interval and sub-beat slot, which onset
// shape (kick-ish, snare-ish, hat-ish, ...) it expects and how loud.  The gap's
// onsets are classified the same way, so a placement is scored by how many
// expected hits land on an onset of the expected shape -- a half-beat-off
// placement puts kick slots on hat onsets and scores badly.  Chroma still
// contributes, weighted by (1 - rhythm_weight), to say "same section".

static void rhythm_prepare(FillCtx* ctx) {
    const ShapeAnalysis* sa = ctx->shapes;
    ctx->so0 = ctx->so1 = 0;
    if (!sa || !sa->vocab.valid) return;
    ctx->so0 = shape_onset_at(*sa, ctx->gap.t0 - ctx->period);
    ctx->so1 = shape_onset_at(*sa, ctx->gap.t1 + 2.0 * ctx->period);
}

// Soft shape distribution and loudness of the gap onset nearest to slot
// `s` of interval [a, b) (within +-1 slot); presence 0 when none.
static float slot_observe(const FillCtx& ctx, double a, double b, int S, int s,
                          const float** soft_out)
{
    const ShapeAnalysis* sa = ctx.shapes;
    double len  = b - a;
    double tc   = a + (s + 0.5) * len / S;
    double tol  = 1.0 * len / S;            // +-1 slot
    int i0 = shape_onset_at(*sa, tc - tol);
    float best = 0.0f; const float* bs = nullptr;
    for (int i = i0; i < (int)sa->onset_t.size() && sa->onset_t[i] <= tc + tol; i++) {
        float d = (float)fabs(sa->onset_t[i] - tc) / (float)tol;
        float pres = 1.0f - 0.5f * d;
        if (pres > best) { best = pres; bs = &sa->onset_soft[(size_t)i * sa->vocab.k]; }
    }
    *soft_out = bs;
    return best;
}

static bool rhythm_place(const FillCtx& ctx, const Template& t, double start, double r,
                         int fitted, PlaceScore* out)
{
    const ShapeAnalysis* sa = ctx.shapes;
    if (!sa || !sa->vocab.valid || t.pattern.empty()) return chroma_place(ctx, t, start, r, fitted, out);
    int S = t.slots, K = sa->vocab.k;
    int stride = 1 + K;
    float match = 0, expected = 0, miss = 0, extra = 0;
    for (int k = 0; k < fitted; k++) {
        double a = start + r * t.off[k], b = start + r * t.off[k + 1];
        for (int s = 0; s < S; s++) {
            const float* pat = &t.pattern[((size_t)k * S + s) * stride];
            float e = pat[0];
            const float* os = nullptr;
            float o = slot_observe(ctx, a, b, S, s, &os);
            if (e > 0.05f) {
                expected += e;
                if (o > 0.0f && os) {
                    // Cosine of the two soft memberships: ~1 when the same
                    // shape wins on both sides, ~0 for clearly different hits
                    float dot = 0, na = 0, nb = 0;
                    for (int c = 0; c < K; c++) { dot += pat[1 + c] * os[c]; na += pat[1 + c] * pat[1 + c]; nb += os[c] * os[c]; }
                    float cs = (na > 1e-9f && nb > 1e-9f) ? dot / sqrtf(na * nb) : 0.0f;
                    match += e * o * cs;
                } else {
                    miss += e;
                }
            } else if (o > 0.5f) {
                extra += o;
            }
        }
    }
    if (expected < 1e-3f) return chroma_place(ctx, t, start, r, fitted, out);
    float rhythm = (match - ctx.p->miss_penalty * miss - ctx.p->extra_penalty * extra) / expected;
    if (rhythm < 0.0f) rhythm = 0.0f;
    if (rhythm > 1.0f) rhythm = 1.0f;
    // Chroma says "same material"; rhythm is a bonus that says "and this is
    // the phase".  Additive, so a section whose drum pattern differs from the
    // template still transfers on chroma alone -- just without the bonus.
    PlaceScore base;
    chroma_place(ctx, t, start, r, fitted, &base);
    out->sim = base.sim; out->rhythm = rhythm;
    out->score = base.score + ctx.p->rhythm_weight * rhythm;
    return true;
}

static double rhythm_snap(const FillCtx& ctx, const Template* t, int k, double at, bool* found) {
    const ShapeAnalysis* sa = ctx.shapes;
    double win = ctx.p->onset_window * ctx.period;
    *found = false;
    if (!sa || !sa->vocab.valid) return nearest_onset(ctx.onsets, at, win, found);
    // What the template expects on this beat (slot 0 of interval k)
    const float* expect = nullptr;
    float e = 0.0f;
    if (t && !t->pattern.empty() && k >= 0 && k < (int)t->off.size() - 1) {
        const float* pat = &t->pattern[((size_t)k * t->slots) * (1 + sa->vocab.k)];
        e = pat[0]; expect = pat + 1;
    }
    int i0 = shape_onset_at(*sa, at - win);
    double best_t = at; float best_v = -1.0f;
    double near_t = at; float near_v = -1.0f;       // fallback: nearest onset of any shape
    for (int i = i0; i < (int)sa->onset_t.size() && sa->onset_t[i] <= at + win; i++) {
        float d = (float)(fabs(sa->onset_t[i] - at) / win);
        if (1.0f - d > near_v) { near_v = 1.0f - d; near_t = sa->onset_t[i]; }
        if (expect && e > 0.05f) {
            float dot = 0; for (int c = 0; c < sa->vocab.k; c++) dot += expect[c] * sa->onset_soft[(size_t)i * sa->vocab.k + c];
            if (dot < 0.3f) continue;                // wrong kind of hit for this slot
            float v = dot - 0.5f * d;
            if (v > best_v) { best_v = v; best_t = sa->onset_t[i]; }
        }
    }
    if (best_v >= 0.0f) { *found = true; return best_t; }
    // No onset of the expected shape: the nearest hit still beats drifting
    if (near_v >= 0.0f) { *found = true; return near_t; }
    return at;
}

const BeatFillAlgo BEAT_FILL_ALGOS[] = {
    { "Chroma transfer",
      "Match a mapped stretch to the gap by pitch-class similarity and bring its beats over",
      chroma_prepare, chroma_place, chroma_snap },
    { "Rhythm-shape transfer",
      "Classify onsets into track-specific timbre shapes; match the mapped rhythm pattern\n"
      "(which shape hits on which sub-beat) plus chroma, and snap beats to onsets of the\n"
      "expected shape",
      rhythm_prepare, rhythm_place, rhythm_snap },
    { "Anchor + region growing",
      "Grow the beat grid outward from high-confidence anchors -- strong template matches,\n"
      "the gap's edges, and periodicity-supported onsets -- then reconcile where regions\n"
      "meet, instead of one greedy left-to-right chain.  Several anchor scenarios are\n"
      "grown to complete outcomes and the best-scoring result wins.",
      rhythm_prepare, rhythm_place, rhythm_snap },
};
const int BEAT_FILL_ALGO_COUNT = (int)(sizeof(BEAT_FILL_ALGOS) / sizeof(BEAT_FILL_ALGOS[0]));

const char* complete_fill_algo_name(int idx) {
    return (idx >= 0 && idx < BEAT_FILL_ALGO_COUNT) ? BEAT_FILL_ALGOS[idx].name : "?";
}
const char* complete_fill_algo_tip(int idx) {
    return (idx >= 0 && idx < BEAT_FILL_ALGO_COUNT) ? BEAT_FILL_ALGOS[idx].tip : "";
}
int complete_fill_algo_count() { return BEAT_FILL_ALGO_COUNT; }

// ===========================================================================
// Beat completion (shared driver)
// ===========================================================================

struct Fit {            // best template placement found at a position
    int    tmpl;        // -1 = none
    double start;       // time of the template's first beat
    double r;           // stretch factor
    int    fitted;      // intervals that fit in the gap
    float  sim, rhythm;
    float  score;
};

static Fit best_fit_at(const FillCtx& ctx, const BeatFillAlgo& algo,
                       double pos, double gap_end, bool pos_is_anchor)
{
    const CompleteParams& p = *ctx.p;
    double period = ctx.period;
    Fit best; best.tmpl = -1; best.score = -1.0f; best.sim = 0; best.rhythm = 0; best.fitted = 0;
    best.start = pos; best.r = 1.0;
    int   jn  = (pos_is_anchor || p.jitter_beats <= 0.0f) ? 0 : 2;   // sub-beat slack steps each side
    double jd = p.jitter_beats * period / (jn ? jn : 1);
    int   la  = p.lookahead_beats < 0 ? 0 : p.lookahead_beats;          // whole beats ahead
    int   ws  = p.warp_steps < 1 ? 1 : p.warp_steps;

    for (size_t ti = 0; ti < ctx.tm->size(); ti++) {
        const Template& t = (*ctx.tm)[ti];
        int n_int = (int)t.off.size() - 1;
        if (n_int < 1) continue;
        for (int wi = 0; wi < ws; wi++) {
            double r = (ws == 1) ? 1.0 : 1.0 - p.max_warp + 2.0 * p.max_warp * wi / (ws - 1);
            for (int oi = -jn; oi <= jn + la; oi++) {
                // oi in [-jn, jn]: sub-beat slack; beyond: whole beats of lookahead
                int    ahead = oi > jn ? oi - jn : 0;
                double start = pos + oi * jd;
                if (ahead) { start = pos; for (int a = 0; a < ahead; a++) start = next_grid_beat(ctx, start, period); }
                float  ahead_pen = p.lookahead_penalty * ahead;
                int fitted = 0;
                for (int k = 0; k < n_int; k++) {
                    if (start + r * t.off[k + 1] <= gap_end + 0.5 * period) fitted = k + 1;
                    else break;
                }
                int need = std::max(p.min_template_beats - 1, (n_int + 1) / 2);
                if (fitted < need) continue;
                PlaceScore ps;
                if (!algo.place(ctx, t, start, r, fitted, &ps)) continue;
                ps.score -= ahead_pen;
                if (ps.score > best.score) {
                    best.tmpl = (int)ti; best.start = start; best.r = r;
                    best.fitted = fitted; best.sim = ps.sim; best.rhythm = ps.rhythm;
                    best.score = ps.score;
                }
            }
        }
    }
    return best;
}

// One stretch of proposed beats inside a gap, before snapping/smoothing.
struct Segment {
    int    tmpl;          // >= 0 template; -1 tempo fill / bridge; -2 grown from an anchor
    double r;
    float  sim, rhythm, score;
    int    first, n;      // into the gap's beat list
    bool   seam = false;  // bridge between regions that disagree in phase (grid break?)
};

static void detect_onsets(const CompleteInputs& in, const CompleteParams& p,
                          double t0, double t1, std::vector<double>* onsets,
                          std::vector<double>* det_beats)
{
    onsets->clear();
    det_beats->clear();
    if (!in.audio.pcm || p.beat_algo_idx < 0 || p.beat_algo_idx >= BEAT_ALGO_COUNT) return;
    if (t0 < 0) t0 = 0;
    if (t1 > in.duration) t1 = in.duration;
    if (t1 - t0 < 0.2) return;

    // Seeds: mapped beats just outside the window so the detector keeps phase
    static double seeds[64];
    int ns = 0;
    const BeatMap* bm = in.beatmap;
    int il = first_beat_at(bm, t0);
    for (int i = std::max(0, il - 16); i < bm->count && ns < 64; i++) {
        double t = bm->beats[i].time;
        if (t > t1 + 16.0) break;
        if (t >= t0 - 16.0) seeds[ns++] = t;
    }

    BeatAlgoParams bp = {};
    bp.min_bpm = p.det_min_bpm; bp.max_bpm = p.det_max_bpm;
    bp.onset_threshold = p.det_threshold; bp.dp_tightness = p.det_tightness;
    bp.pre_onset_ms = 0.0f;
    bp.seed_times = ns >= 2 ? seeds : nullptr;
    bp.seed_count = ns >= 2 ? ns : 0;

    static AutoBeatList ab;   // large; keep off the stack
    autobeat_init(&ab);
    BEAT_ALGOS[p.beat_algo_idx].fn(in.audio.pcm, in.audio.frame_count, in.audio.channels,
                                   in.audio.sample_rate, t0, t1, &bp, &ab);
    onsets->assign(ab.onset_times, ab.onset_times + ab.onset_count);
    std::sort(onsets->begin(), onsets->end());
    det_beats->assign(ab.beat_times, ab.beat_times + ab.beat_count);
    std::sort(det_beats->begin(), det_beats->end());
}

// ===========================================================================
// Anchor beats + region growing (automation.md §5)
// ===========================================================================
//
// Instead of one greedy left-to-right chain, the gap is filled from many
// high-confidence anchors grown outward:
//   - anchor regions: template placements scored anywhere in the gap, kept by
//     non-maximum suppression;
//   - anchor beats: onsets with strong periodic support, seeding stretches no
//     other anchor covers;
//   - the gap's edges, which are mapped beats and so anchors of confidence 1.
// Regions grow beat by beat both ways, snapping to onsets and losing
// confidence where the audio offers none.  Where two regions meet, the seam
// is reconciled: the space between is bridged with evenly spaced beats (the
// shared redistribution pass then splits the residual), and a bridge whose
// length is far from a whole number of beats is flagged as a possible grid
// break -- a genuine tempo discontinuity stays visible instead of dragging
// everything after it.
//
// Several anchor *scenarios* (which anchors to trust) are grown to complete
// outcomes and the finished results ranked -- choosing is not committing;
// the growth is cheap enough to try alternatives and keep the best.

// Template placement at a fixed position: no sub-beat jitter, no lookahead.
static Fit anchor_fit_at(const FillCtx& ctx, const BeatFillAlgo& algo,
                         double pos, double gap_end)
{
    const CompleteParams& p = *ctx.p;
    Fit best; best.tmpl = -1; best.score = -1.0f; best.sim = 0; best.rhythm = 0;
    best.fitted = 0; best.start = pos; best.r = 1.0;
    int ws = p.warp_steps < 1 ? 1 : p.warp_steps;
    for (size_t ti = 0; ti < ctx.tm->size(); ti++) {
        const Template& t = (*ctx.tm)[ti];
        int n_int = (int)t.off.size() - 1;
        if (n_int < 1) continue;
        for (int wi = 0; wi < ws; wi++) {
            double r = (ws == 1) ? 1.0 : 1.0 - p.max_warp + 2.0 * p.max_warp * wi / (ws - 1);
            int fitted = 0;
            for (int k = 0; k < n_int; k++) {
                if (pos + r * t.off[k + 1] <= gap_end + 0.5 * ctx.period) fitted = k + 1;
                else break;
            }
            int need = std::max(p.min_template_beats - 1, (n_int + 1) / 2);
            if (fitted < need) continue;
            PlaceScore ps;
            if (!algo.place(ctx, t, pos, r, fitted, &ps)) continue;
            if (ps.score > best.score) {
                best.tmpl = (int)ti; best.start = pos; best.r = r;
                best.fitted = fitted; best.sim = ps.sim; best.rhythm = ps.rhythm;
                best.score = ps.score;
            }
        }
    }
    return best;
}

// One grown region: an anchored core (template beats, a periodicity-supported
// onset, or a gap edge) plus the beats grown outward from it.
struct GrowRegion {
    std::vector<double> bt;      // beats, sorted
    std::vector<int>    bk;      // template beat index per beat (-1 = grown)
    int    tmpl = -1;            // >= 0: template anchor region
    double r = 1.0;
    float  sim = 0, rhythm = 0, score = 0;
    int    left_ext = 0;         // grown beats beyond the anchored core, per side
    int    right_ext = 0;
    double fl = 0, fr = 0;       // frontier times (the anchor position when empty)
    double per_l = 0, per_r = 0; // frontier periods
    float  conf_l = 0, conf_r = 0;
    bool   done_l = false, done_r = false;
};

// Which anchors a growth run trusts.
struct AnchorScenario {
    const char* name;
    bool  templates;      // use template anchor regions
    bool  onset_anchors;  // seed uncovered stretches from periodic onsets
    float thr_bonus;      // added to the template acceptance threshold
    int   skip_best;      // ignore the N best template fits (alternate tiling)
};

static void anchor_grow_propose(FillCtx& ctx, const BeatFillAlgo& algo,
                                const std::vector<Template>& tm,
                                const std::vector<Fit>& fits_sorted,
                                const AnchorScenario& sc,
                                std::vector<double>* beats, std::vector<int>* beat_tmpl,
                                std::vector<int>* beat_k, std::vector<Segment>* segs,
                                float* seam_acc)
{
    *seam_acc = 0.0f;
    const CompleteParams& p = *ctx.p;
    const Gap& g = ctx.gap;
    const double period = ctx.period;
    const double win = std::max(1e-6, p.onset_window * period);
    std::vector<GrowRegion> regs;

    auto local_per = [&](double t) {
        double frac = (t - g.t0) / std::max(1e-6, g.t1 - g.t0);
        double per  = g.period_l + (g.period_r - g.period_l) * frac;
        return per > 0 ? per : period;
    };

    // --- Edge anchors: the gap's bounding mapped beats, confidence 1 ---
    if (g.has_l) {
        GrowRegion R; R.fl = R.fr = g.t0;
        R.per_l = R.per_r = g.period_l > 0 ? g.period_l : period;
        R.conf_l = R.conf_r = 1.0f; R.done_l = true;
        regs.push_back(R);
    }
    if (g.has_r) {
        GrowRegion R; R.fl = R.fr = g.t1;
        R.per_l = R.per_r = g.period_r > 0 ? g.period_r : period;
        R.conf_l = R.conf_r = 1.0f; R.done_r = true;
        regs.push_back(R);
    }

    // --- Template anchor regions, by non-maximum suppression on score ---
    if (sc.templates) {
        int skipped = 0;
        for (const Fit& f : fits_sorted) {
            if (f.score < p.beat_sim_threshold + sc.thr_bonus) continue;
            if (skipped < sc.skip_best) { skipped++; continue; }
            const Template& t = tm[f.tmpl];
            double f0 = f.start, f1 = f.start + f.r * t.off[f.fitted];
            bool clash = false;
            for (const GrowRegion& R : regs) {
                if (R.fr <= R.fl) continue;                  // edge anchors: no span
                double o = std::min(f1, R.fr) - std::max(f0, R.fl);
                if (o > 0.2 * (f1 - f0)) { clash = true; break; }
            }
            if (clash) continue;
            GrowRegion R;
            R.tmpl = f.tmpl; R.r = f.r; R.sim = f.sim; R.rhythm = f.rhythm;
            R.score = std::max(0.0f, std::min(1.0f, f.score));
            double lo = g.has_l ? g.t0 + 0.4 * period : g.t0 - 1e-9;
            double hi = g.has_r ? g.t1 - 0.4 * period : g.t1 + 1e-9;
            for (int k = 0; k <= f.fitted; k++) {
                double bt = f.start + f.r * t.off[k];
                if (bt < lo) continue;
                if (bt > hi) break;
                R.bt.push_back(bt); R.bk.push_back(k);
            }
            if ((int)R.bt.size() < 2) continue;
            int n_int = (int)t.off.size() - 1;
            double per_t = n_int > 0 ? f.r * t.off[n_int] / n_int : period;
            R.fl = R.bt.front(); R.fr = R.bt.back();
            R.per_l = R.per_r = per_t > 0 ? per_t : period;
            R.conf_l = R.conf_r = 0.5f + 0.4f * R.score;
            regs.push_back(R);
        }
    }

    // --- Onset anchor beats: periodicity-supported onsets seeding stretches
    // no other anchor covers ---
    if (sc.onset_anchors) {
        std::vector<std::pair<double, double>> covered;
        for (const GrowRegion& R : regs) covered.push_back({ R.fl, R.fr });
        std::sort(covered.begin(), covered.end());
        auto seed_stretch = [&](double a, double b) {
            if (b - a < 8.0 * period) return;
            struct OA { double t; int support; };
            std::vector<OA> cands;
            for (double o : ctx.onsets) {
                if (o < a + 2.0 * period || o > b - 2.0 * period) continue;
                double per_u = local_per(o);
                int sup = 0;
                for (int m = -2; m <= 2; m++) {
                    if (!m) continue;
                    bool fnd;
                    nearest_onset(ctx.onsets, o + m * per_u, 0.15 * per_u, &fnd);
                    if (fnd) sup++;
                }
                // Prefer onsets the detector also called a beat: the seed
                // then carries a phase, not just a period.
                bool on_det;
                nearest_onset(ctx.det, o, 0.2 * per_u, &on_det);
                if (on_det) sup += 2;
                if (sup >= 3) cands.push_back({ o, sup });
            }
            std::sort(cands.begin(), cands.end(),
                      [](const OA& x, const OA& y) { return x.support > y.support; });
            std::vector<double> placed;
            for (const OA& oa : cands) {
                bool near = false;
                for (double q : placed)
                    if (fabs(q - oa.t) < 6.0 * period) { near = true; break; }
                if (near) continue;
                placed.push_back(oa.t);
                GrowRegion R;
                R.bt.push_back(oa.t); R.bk.push_back(-1);
                R.fl = R.fr = oa.t;
                R.per_l = R.per_r = local_per(oa.t);
                R.conf_l = R.conf_r = std::min(0.75f, 0.55f + 0.05f * (oa.support - 3));
                regs.push_back(R);
            }
        };
        double u0 = g.t0;
        for (const auto& c : covered) {
            if (c.first > u0) seed_stretch(u0, c.first);
            u0 = std::max(u0, c.second);
        }
        seed_stretch(u0, g.t1);
    }

    // --- Region growing: every open frontier extends one beat per round,
    // snapping to onsets, until it runs out of confidence, hits a bound, or
    // reaches another region's territory ---
    auto blocked = [&](const GrowRegion* self, double t) {
        for (const GrowRegion& R : regs) {
            if (&R == self) continue;
            if (t >= R.fl - 0.55 * period && t <= R.fr + 0.55 * period) return true;
        }
        return false;
    };
    bool any = true;
    int  guard = 0;
    while (any && guard++ < 4000) {
        any = false;
        for (GrowRegion& R : regs) {
            if (!R.done_r) {
                double per  = R.per_r > 0 ? R.per_r : period;
                double pred = p.grid_follow ? next_grid_beat(ctx, R.fr, per) : R.fr + per;
                if (pred < R.fr + 0.5 * per) pred = R.fr + per;
                double hi = g.has_r ? g.t1 - 0.5 * period : g.t1 + 1e-9;
                if (pred > hi || blocked(&R, pred)) {
                    R.done_r = true;
                } else {
                    bool found;
                    double sn = algo.snap(ctx, nullptr, -1, pred, &found);
                    double nb; float c = R.conf_r;
                    if (found) {
                        double dist = fabs(sn - pred) / win;
                        nb = pred + p.onset_weight * (sn - pred);
                        c  = 0.6f * c + 0.4f * (float)(1.0 - 0.5 * std::min(1.0, dist));
                        // Track tempo, but leashed to the edges' period: an
                        // eighth-note-dense detector grid must not walk the
                        // period down step by step.
                        double lp = local_per(nb);
                        R.per_r = std::min(1.25 * lp, std::max(0.8 * lp,
                                           0.75 * per + 0.25 * (nb - R.fr)));
                    } else {
                        nb = pred; c *= 0.78f;
                    }
                    if (nb <= R.fr + 0.4 * per) {
                        R.done_r = true;
                    } else {
                        R.bt.push_back(nb); R.bk.push_back(-1);
                        if (R.tmpl >= 0) R.right_ext++;
                        R.fr = nb; R.conf_r = c; any = true;
                    }
                }
            }
            if (!R.done_l) {
                double per  = R.per_l > 0 ? R.per_l : period;
                double pred = p.grid_follow ? prev_grid_beat(ctx, R.fl, per) : R.fl - per;
                if (pred > R.fl - 0.5 * per) pred = R.fl - per;
                double lo = g.has_l ? g.t0 + 0.5 * period : g.t0 - 1e-9;
                if (pred < lo || blocked(&R, pred)) {
                    R.done_l = true;
                } else {
                    bool found;
                    double sn = algo.snap(ctx, nullptr, -1, pred, &found);
                    double nb; float c = R.conf_l;
                    if (found) {
                        double dist = fabs(sn - pred) / win;
                        nb = pred + p.onset_weight * (sn - pred);
                        c  = 0.6f * c + 0.4f * (float)(1.0 - 0.5 * std::min(1.0, dist));
                        double lp = local_per(nb);
                        R.per_l = std::min(1.25 * lp, std::max(0.8 * lp,
                                           0.75 * per + 0.25 * (R.fl - nb)));
                    } else {
                        nb = pred; c *= 0.78f;
                    }
                    if (nb >= R.fl - 0.4 * per) {
                        R.done_l = true;
                    } else {
                        R.bt.insert(R.bt.begin(), nb); R.bk.insert(R.bk.begin(), -1);
                        if (R.tmpl >= 0) R.left_ext++;
                        R.fl = nb; R.conf_l = c; any = true;
                    }
                }
            }
        }
    }

    // --- Assembly: regions left to right; the space between neighbours is
    // bridged with evenly spaced beats (interpolating from both sides), and a
    // bridge far from a whole number of beats is flagged as a grid break ---
    std::vector<const GrowRegion*> chunks;
    for (const GrowRegion& R : regs)
        if (!R.bt.empty()) chunks.push_back(&R);
    std::sort(chunks.begin(), chunks.end(),
              [](const GrowRegion* a, const GrowRegion* b) { return a->bt.front() < b->bt.front(); });

    double prev_t    = g.t0;
    double prev_per  = g.period_l > 0 ? g.period_l : period;
    bool   have_prev = g.has_l;

    auto push = [&](double t, int tmpl, int k) {
        beats->push_back(t); beat_tmpl->push_back(tmpl); beat_k->push_back(k);
    };
    auto last_placed = [&]() {
        return beats->empty() ? (have_prev ? prev_t : -1e300) : beats->back();
    };
    auto bridge_to = [&](double t_to, double per_to) {
        double from = last_placed();
        if (from < -1e200) return;
        double d = t_to - from;
        double pm = 0.5 * ((prev_per > 0 ? prev_per : period) + (per_to > 0 ? per_to : period));
        if (pm <= 0) pm = period;
        int k = (int)floor(d / pm + 0.5);
        if (k < 1) k = 1;
        double mism = fabs(d - k * pm) / pm;
        // Every meeting of two regions contributes its phase disagreement to
        // the outcome ranking -- the gap's mapped edges are the phase truth,
        // so a mis-phased anchor shows up here as seams against its
        // neighbours even when nothing needs inserting.
        *seam_acc += (float)std::min(0.5, std::max(0.0, mism - 0.1));
        if (k >= 2) {
            Segment s; s.tmpl = -1; s.r = 1.0; s.sim = 0; s.rhythm = 0;
            s.seam  = mism > 0.25;
            s.score = s.seam ? 0.3f : 0.4f;
            s.first = (int)beats->size();
            for (int i = 1; i < k; i++) push(from + d * i / k, -1, -1);
            s.n = (int)beats->size() - s.first;
            if (s.n > 0) segs->push_back(s);
        }
    };

    // Open head with no left edge: cover it at low confidence.
    if (!g.has_l && !chunks.empty()) {
        double first_t = chunks[0]->bt.front();
        double per = chunks[0]->per_l > 0 ? chunks[0]->per_l : period;
        if (first_t - g.t0 > 1.5 * per) {
            std::vector<double> hb;
            for (double t = first_t - per; t >= g.t0; t -= per) hb.push_back(t);
            std::reverse(hb.begin(), hb.end());
            Segment s; s.tmpl = -1; s.r = 1.0; s.sim = 0; s.rhythm = 0;
            s.score = 0.3f; s.first = 0;
            for (double t : hb) push(t, -1, -1);
            s.n = (int)beats->size();
            if (s.n > 0) segs->push_back(s);
            have_prev = true;
        }
    }

    for (const GrowRegion* R : chunks) {
        bridge_to(R->bt.front(), R->per_l);
        int n = (int)R->bt.size();
        int core0 = R->tmpl >= 0 ? R->left_ext : 0;
        int core1 = R->tmpl >= 0 ? n - R->right_ext : n;
        auto emit_span = [&](int i0, int i1, int tmpl_id) {
            int first = (int)beats->size();
            for (int i = i0; i < i1; i++) {
                double t = R->bt[i];
                if (t <= last_placed() + 0.3 * period) continue;
                push(t, tmpl_id, tmpl_id >= 0 ? R->bk[i] : -1);
            }
            int cnt = (int)beats->size() - first;
            if (cnt > 0) {
                Segment s;
                s.tmpl = tmpl_id; s.r = tmpl_id >= 0 ? R->r : 1.0;
                s.sim = tmpl_id >= 0 ? R->sim : 0;
                s.rhythm = tmpl_id >= 0 ? R->rhythm : 0;
                s.score = tmpl_id >= 0 ? R->score : 0.5f;
                s.first = first; s.n = cnt;
                segs->push_back(s);
            }
        };
        emit_span(0, core0, -2);
        emit_span(core0, core1, R->tmpl >= 0 ? R->tmpl : -2);
        emit_span(core1, n, -2);
        prev_t    = last_placed();
        prev_per  = R->per_r > 0 ? R->per_r : period;
        have_prev = true;
    }

    if (g.has_r) {
        bridge_to(g.t1, g.period_r);
    } else {
        // Open tail: cover what growth did not reach, at low confidence.
        double per  = prev_per > 0 ? prev_per : period;
        double from = last_placed();
        if (from < -1e200) { from = g.t0; have_prev = true; }
        if (g.t1 - from > 1.5 * per) {
            Segment s; s.tmpl = -1; s.r = 1.0; s.sim = 0; s.rhythm = 0;
            s.score = 0.3f; s.first = (int)beats->size();
            double pos = from; int gq = 0;
            while (gq++ < 100000) {
                pos = p.grid_follow ? next_grid_beat(ctx, pos, per) : pos + per;
                if (pos > g.t1) break;
                push(pos, -1, -1);
            }
            s.n = (int)beats->size() - s.first;
            if (s.n > 0) segs->push_back(s);
        }
    }
}

// Rank a completed outcome (any strategy's).  Judged post hoc on the beat
// sequence itself, edges included, so the greedy chain and every growth
// scenario compete on equal terms:
//   - onset support (how much of the audio agrees a beat is there);
//   - phase flips: an interval far from a whole multiple of its local period
//     -- a lattice shifted half a beat against the mapped edges buys exactly
//     one such interval per flip, so each costs dearly;
//   - period sanity against the edges' periods (catches a fill that walked
//     down to eighth notes or up to half time);
//   - tempo smoothness, and a nod toward template-anchored coverage.
static float grow_outcome_score(const FillCtx& ctx, const std::vector<double>& proposed,
                                const std::vector<Segment>& segs)
{
    const Gap& g = ctx.gap;
    if (proposed.empty()) return -1e9f;
    double win = std::max(1e-6, ctx.p->onset_window * ctx.period);
    // Onset support, weighted by the on-beat shape prior when the vocabulary
    // knows one: a half-beat-shifted lattice still finds onsets on eighth-note
    // tracks, but they are the wrong *kind* of onset (hats, not kick/snare),
    // and that is the phase discrimination raw support lacks.
    const ShapeAnalysis* sa = ctx.shapes;
    bool use_prior = sa && sa->vocab.valid && !ctx.beat_prior.empty();
    float support = 0;
    for (double t : proposed) {
        bool f; double o = nearest_onset(ctx.onsets, t, win, &f);
        if (!f) continue;
        float w = 1.0f;
        if (use_prior) {
            int  j = shape_onset_at(*sa, o - win);
            int  bj = -1; double bd = win;
            for (; j < (int)sa->onset_t.size() && sa->onset_t[j] <= o + win; j++) {
                double d = fabs(sa->onset_t[j] - o);
                if (d < bd) { bd = d; bj = j; }
            }
            if (bj >= 0) {
                const float* soft = &sa->onset_soft[(size_t)bj * sa->vocab.k];
                float dot = 0, nn = 0;
                for (int c = 0; c < sa->vocab.k; c++) {
                    dot += soft[c] * ctx.beat_prior[c];
                    nn  += soft[c] * soft[c];
                }
                float cs = (nn > 1e-9f) ? dot / sqrtf(nn) : 0.0f;
                w = 0.4f + 0.6f * std::max(0.0f, std::min(1.0f, cs));
            }
        }
        support += w * (1.0f - 0.5f * (float)(fabs(o - t) / win));
    }
    support /= (float)proposed.size();

    std::vector<double> seq;
    if (g.has_l) seq.push_back(g.t0);
    seq.insert(seq.end(), proposed.begin(), proposed.end());
    if (g.has_r) seq.push_back(g.t1);
    std::vector<double> iv;
    for (size_t i = 1; i < seq.size(); i++) iv.push_back(seq[i] - seq[i - 1]);
    if (iv.empty()) return -1e9f;

    float  flips = 0;
    double rough = 0; int rn = 0;
    for (size_t i = 0; i < iv.size(); i++) {
        size_t a = i >= 4 ? i - 4 : 0;
        size_t b = std::min(iv.size(), i + 5);
        std::vector<double> w(iv.begin() + a, iv.begin() + b);
        std::nth_element(w.begin(), w.begin() + w.size() / 2, w.end());
        double m = w[w.size() / 2];
        if (m > 1e-6) {
            // Only intervals close to a HALF-integer multiple of the local
            // period count as flips: onset-pulled jitter reaches +-40% and
            // must not register, or jittery-but-right loses to smooth-but-
            // half-shifted (which pays only at its two transitions).
            double r  = iv[i] / m;
            double fr = fabs(r - floor(r + 0.5));
            if (fr >= 0.42) flips += 1.0f;
        }
        if (i > 0 && iv[i] > 1e-3 && iv[i - 1] > 1e-3) {
            rough += fabs(log(iv[i] / iv[i - 1]));
            rn++;
        }
    }
    if (rn) rough /= rn;

    double mean_iv = (seq.back() - seq.front()) / (double)iv.size();
    double period_pen = std::max(0.0, fabs(log(mean_iv / ctx.period)) - 0.15);

    // Template quality coverage: beats explained by a template, weighted by
    // how well the scorer liked the placement (independent anchoring earns
    // higher placement scores than a drifting chain).
    double tq = 0; int tot_n = 0;
    for (const Segment& s : segs) {
        if (s.tmpl >= 0) tq += (double)s.score * s.n;
        tot_n += s.n;
    }
    float tmpl_q = tot_n ? (float)(tq / tot_n) : 0.0f;

    if (getenv("COMPLETE_DEBUG"))
        fprintf(stderr, "    [outcome: sup %.3f tq %.3f rough %.3f flips %.0f ppen %.3f]\n",
                support, tmpl_q, rough, flips, period_pen);
    // Flips stay a mild per-event cost: a chain that *drifts* into the wrong
    // phase shows no abrupt interval at all, so onset support must be able to
    // outvote a couple of honest seams.
    return support + 0.35f * tmpl_q - 0.5f * (float)rough
         - 0.02f * flips - 1.0f * (float)std::min(1.0, period_pen);
}

static void fill_gap(const CompleteInputs& in, const CompleteParams& p,
                     const std::vector<Template>& tm, const ShapeAnalysis* shapes,
                     const Gap& g, CompleteProposal* out)
{
    int ai = p.fill_algo_idx;
    if (ai < 0 || ai >= BEAT_FILL_ALGO_COUNT) ai = 0;
    const BeatFillAlgo& algo = BEAT_FILL_ALGOS[ai];

    FillCtx ctx;
    ctx.in = &in; ctx.p = &p; ctx.tm = &tm; ctx.shapes = shapes; ctx.gap = g;
    ctx.period = 0.5 * (g.period_l + g.period_r);
    if (ctx.period <= 0.0) ctx.period = 0.5;
    double period = ctx.period;

    build_frames(in.audio, p.chroma, std::max(0.0, g.t0 - period),
                 std::min(in.duration, g.t1 + 2.0 * period), period, &ctx.fg);
    detect_onsets(in, p, g.t0 - period, g.t1 + period, &ctx.onsets, &ctx.det);
    algo.prepare(&ctx);

    std::vector<double>  beats;      // proposed, excluding anchors
    std::vector<int>     beat_tmpl;  // template index per beat (-1 tempo, -2 grown)
    std::vector<int>     beat_k;     // template beat index per beat
    std::vector<Segment> segs;

    // Greedy left-to-right chain: the native driver for strategies 0 and 1,
    // and one of the ranked candidate outcomes for strategy 2.
    auto run_greedy = [&](std::vector<double>& beats, std::vector<int>& beat_tmpl,
                          std::vector<int>& beat_k, std::vector<Segment>& segs) {
    double period = ctx.period;

    double pos = g.t0;
    bool   at_anchor = g.has_l;
    int    tempo_first = -1;
    double end_limit = g.has_r ? g.t1 - 0.5 * period : g.t1;

    auto push_beat = [&](double t, int tmpl, int k) {
        beats.push_back(t); beat_tmpl.push_back(tmpl); beat_k.push_back(k);
    };
    auto close_tempo_run = [&]() {
        if (tempo_first < 0) return;
        Segment s; s.tmpl = -1; s.r = 1.0; s.sim = 0; s.rhythm = 0; s.score = 0.4f;
        s.first = tempo_first; s.n = (int)beats.size() - tempo_first;
        if (s.n > 0) segs.push_back(s);
        tempo_first = -1;
    };

    int guard = 0;
    while (pos < end_limit && guard++ < 100000) {
        Fit f;
        f.tmpl = -1;
        if (!tm.empty())
            f = best_fit_at(ctx, algo, pos, g.t1, at_anchor || pos == g.t0);

        if (getenv("COMPLETE_DEBUG") && guard == 1 && !tm.empty() && g.has_l) {
            for (int q = -6; q <= 6; q++) {
                PlaceScore ps;
                double st = pos + q * ctx.period / 12.0;
                int n_int = (int)tm[0].off.size() - 1;
                if (algo.place(ctx, tm[0], st, 1.0, n_int, &ps))
                    fprintf(stderr, "    offset %+d/12 beat: sim %.2f rhythm %.2f score %.2f\n", q, ps.sim, ps.rhythm, ps.score);
            }
        }
        if (getenv("COMPLETE_DEBUG") && guard < 12)
            fprintf(stderr, "  pos %.2f: tmpl %d start %.3f r %.3f fitted %d sim %.2f rhythm %.2f score %.2f\n",
                    pos, f.tmpl, f.start, f.r, f.fitted, f.sim, f.rhythm, f.score);
        if (f.tmpl >= 0 && f.score >= p.beat_sim_threshold) {
            // Lookahead: tempo beats carry the grid up to where the template starts
            while (f.start - pos > 1.5 * period) {
                if (tempo_first < 0) tempo_first = (int)beats.size();
                push_beat(next_grid_beat(ctx, pos, period), -1, -1);
                pos = beats.back();
                at_anchor = false;
            }
            close_tempo_run();
            const Template& t = tm[f.tmpl];
            Segment s; s.tmpl = f.tmpl; s.r = f.r; s.sim = f.sim; s.rhythm = f.rhythm; s.score = f.score;
            s.first = (int)beats.size();
            int k0 = (fabs(f.start - pos) < 1e-6 && (at_anchor || !beats.empty())) ? 1 : 0;
            for (int k = k0; k <= f.fitted; k++) {
                double bt = f.start + f.r * t.off[k];
                if (bt <= pos + 1e-6) continue;
                if (g.has_r && bt > g.t1 - 0.5 * period) break;
                push_beat(bt, f.tmpl, k);
            }
            s.n = (int)beats.size() - s.first;
            if (s.n <= 0) {
                if (tempo_first < 0) tempo_first = (int)beats.size();
                push_beat(pos + period, -1, -1);
            } else {
                segs.push_back(s);
                int n_int = (int)t.off.size() - 1;
                if (n_int > 0) period = f.r * t.off[n_int] / n_int;
            }
            pos = beats.back();
            at_anchor = false;
        } else {
            if (tempo_first < 0) tempo_first = (int)beats.size();
            double frac = (pos - g.t0) / std::max(1e-6, g.t1 - g.t0);
            double per  = g.period_l + (g.period_r - g.period_l) * frac;
            if (per <= 0) per = period;
            push_beat(p.grid_follow ? next_grid_beat(ctx, pos, per) : pos + per, -1, -1);
            pos = beats.back();
            at_anchor = false;
        }
    }
    close_tempo_run();
    };  // end greedy chain

    if (ai == 2) {
        // --- Anchor beats + region growing ---
        // Template fits are computed once; the greedy chain and each anchor
        // scenario are run to complete outcomes and the finished results are
        // ranked -- choosing a strategy is not committing to it.

        // On-beat shape prior for the outcome ranking: what kinds of onsets
        // the already-mapped beats sit on.
        if (shapes && shapes->vocab.valid) {
            std::vector<float> pr(shapes->vocab.k, 0.0f);
            int n = 0;
            const BeatMap* bm = in.beatmap;
            for (int i = 0; i < bm->count; i++) {
                double t  = bm->beats[i].time;
                int    j0 = shape_onset_at(*shapes, t - 0.1 * ctx.period);
                for (int j = j0; j < (int)shapes->onset_t.size() &&
                                 shapes->onset_t[j] <= t + 0.1 * ctx.period; j++) {
                    for (int c = 0; c < shapes->vocab.k; c++)
                        pr[c] += shapes->onset_soft[(size_t)j * shapes->vocab.k + c];
                    n++;
                }
            }
            if (n >= 8) {
                float nn = 0;
                for (float v : pr) nn += v * v;
                if (nn > 1e-9f) {
                    for (float& v : pr) v /= sqrtf(nn);
                    ctx.beat_prior = pr;
                }
            }
        }

        std::vector<double> starts;
        if (g.has_l) starts.push_back(g.t0);
        if (!ctx.det.empty()) {
            for (double t : ctx.det)
                if (t > g.t0 + 0.25 * period && t < g.t1 - 0.25 * period) starts.push_back(t);
        } else {
            for (double t = g.t0 + period; t < g.t1 - period; t += period) starts.push_back(t);
        }
        std::vector<Fit> fits;
        if (!tm.empty())
            for (double s0 : starts) {
                Fit f = anchor_fit_at(ctx, algo, s0, g.t1);
                if (f.tmpl >= 0 && f.score >= p.beat_sim_threshold) fits.push_back(f);
            }
        // Phase refinement: the candidate starts came from the detector's
        // grid, which can sit off the true beat; rescore each accepted fit at
        // small offsets around its start so an anchor carries the phase the
        // scorer likes best, not the detector's.
        for (Fit& f : fits) {
            const Template& t = tm[f.tmpl];
            for (int q = -3; q <= 3; q++) {
                if (!q) continue;
                double st = f.start + q * period / 6.0;
                PlaceScore ps;
                if (!algo.place(ctx, t, st, f.r, f.fitted, &ps)) continue;
                if (ps.score > f.score) {
                    f.score = ps.score; f.start = st;
                    f.sim = ps.sim; f.rhythm = ps.rhythm;
                }
            }
        }
        std::sort(fits.begin(), fits.end(),
                  [](const Fit& a, const Fit& b) { return a.score > b.score; });

        static const AnchorScenario SCENARIOS[] = {
            { "full",           true,  true,  0.00f, 0 },
            { "no-onset-seeds", true,  false, 0.00f, 0 },
            { "strict-anchors", true,  true,  0.10f, 0 },
            { "no-templates",   false, true,  0.00f, 0 },
            { "edges-only",     false, false, 0.00f, 0 },
            { "alt-tiling",     true,  true,  0.00f, 1 },
        };
        const int NSC = (int)(sizeof(SCENARIOS) / sizeof(SCENARIOS[0]));
        float best_sc = -1e18f;
        {
            // The greedy chain competes on the same outcome score: where it
            // is right (clean template runs) it wins; where it drifts, a
            // grown alternative overtakes it.
            std::vector<double> b; std::vector<int> t2, k2; std::vector<Segment> sg;
            run_greedy(b, t2, k2, sg);
            float score = grow_outcome_score(ctx, b, sg);
            if (getenv("COMPLETE_DEBUG"))
                fprintf(stderr, "  anchor scenario %-14s: %3d beats %2d segs  outcome %.3f\n",
                        "greedy-chain", (int)b.size(), (int)sg.size(), score);
            if (score > best_sc) {
                best_sc = score;
                beats.swap(b); beat_tmpl.swap(t2); beat_k.swap(k2); segs.swap(sg);
            }
        }
        for (int si = 0; si < NSC; si++) {
            const AnchorScenario& sc = SCENARIOS[si];
            if (fits.empty() && si > 0 && !sc.onset_anchors) continue;   // duplicates "full"
            std::vector<double> b; std::vector<int> t2, k2; std::vector<Segment> sg;
            float seam_acc = 0.0f;
            anchor_grow_propose(ctx, algo, tm, fits, sc, &b, &t2, &k2, &sg, &seam_acc);
            float score = grow_outcome_score(ctx, b, sg);
            if (getenv("COMPLETE_DEBUG"))
                fprintf(stderr, "  anchor scenario %-14s: %3d beats %2d segs  seams %.2f  outcome %.3f\n",
                        sc.name, (int)b.size(), (int)sg.size(), seam_acc, score);
            if (score > best_sc) {
                best_sc = score;
                beats.swap(b); beat_tmpl.swap(t2); beat_k.swap(k2); segs.swap(sg);
            }
        }
    } else {
        run_greedy(beats, beat_tmpl, beat_k, segs);
    }

    // Seam repair: a template allowed to start off the grid can leave a
    // half-beat interval against the beat before it.  Drop the offending beat
    // (a tempo-fill beat by preference, else the later one).
    {
        auto seg_of = [&](int bi) -> int {
            for (size_t k = 0; k < segs.size(); k++)
                if (bi >= segs[k].first && bi < segs[k].first + segs[k].n) return (int)k;
            return -1;
        };
        auto drop = [&](int bi) {
            int sk = seg_of(bi);
            beats.erase(beats.begin() + bi);
            beat_tmpl.erase(beat_tmpl.begin() + bi);
            beat_k.erase(beat_k.begin() + bi);
            for (size_t k = 0; k < segs.size(); k++) {
                if ((int)k == sk) segs[k].n--;
                else if (segs[k].first > bi) segs[k].first--;
            }
        };
        for (int i = 0; i < (int)beats.size(); ) {
            double prev = i > 0 ? beats[i - 1] : (g.has_l ? g.t0 : -1.0);
            if (prev < 0.0) { i++; continue; }
            if (beats[i] - prev >= 0.6 * ctx.period) { i++; continue; }
            int si = seg_of(i), sp = i > 0 ? seg_of(i - 1) : -1;
            bool cur_tempo  = si >= 0 && segs[si].tmpl < 0;
            bool prev_tempo = sp >= 0 && segs[sp].tmpl < 0;
            if (i > 0 && prev_tempo && !cur_tempo) drop(i - 1);
            else                                   drop(i);
        }
        if (g.has_r)
            while (!beats.empty() && g.t1 - beats.back() < 0.6 * ctx.period) drop((int)beats.size() - 1);
        for (size_t k = 0; k < segs.size(); )
            if (segs[k].n <= 0) segs.erase(segs.begin() + k); else k++;
    }

    // Tempo runs and bridges bounded on both sides by known beats:
    // redistribute evenly.  Grown segments (-2) keep their onset-snapped
    // positions -- evening them out would undo the growth.
    for (Segment& s : segs) {
        if (s.tmpl != -1 || s.n <= 0) continue;
        bool has_left  = (s.first > 0) || g.has_l;
        double tl = s.first > 0 ? beats[s.first - 1] : g.t0;
        int    ri = s.first + s.n;
        bool   has_right = (ri < (int)beats.size()) || g.has_r;
        double tr = ri < (int)beats.size() ? beats[ri] : g.t1;
        if (!has_left || !has_right) continue;
        double span = tr - tl;
        double per  = s.n > 0 ? (beats[s.first + s.n - 1] - tl) / s.n : period;
        int    k    = (int)floor(span / per + 0.5);
        if (k < 1) k = 1;
        int new_n = k - 1;
        if (new_n != s.n) {
            int delta = new_n - s.n;
            beats.erase(beats.begin() + s.first, beats.begin() + s.first + s.n);
            beat_tmpl.erase(beat_tmpl.begin() + s.first, beat_tmpl.begin() + s.first + s.n);
            beat_k.erase(beat_k.begin() + s.first, beat_k.begin() + s.first + s.n);
            beats.insert(beats.begin() + s.first, new_n, 0.0);
            beat_tmpl.insert(beat_tmpl.begin() + s.first, new_n, -1);
            beat_k.insert(beat_k.begin() + s.first, new_n, -1);
            for (Segment& o : segs) if (o.first > s.first) o.first += delta;
            s.n = new_n;
        }
        for (int i = 0; i < s.n; i++) beats[s.first + i] = tl + span * (i + 1) / k;
    }

    if (beats.empty()) return;

    // Fit each segment to the audio: find the onset each beat would snap to,
    // refit the segment's stretch and offset by least squares over the onsets
    // it found (the template's own spacing is only a prior -- the track's
    // tempo here is what the onsets say), re-place every beat on that line,
    // then pull the ones with an onset the rest of the way.
    std::vector<float>  conf(beats.size(), 0.5f);
    std::vector<double> before = beats;
    std::vector<double> on_t(beats.size(), 0.0);
    std::vector<char>   on_ok(beats.size(), 0);
    auto snap_all = [&](const Segment& s) {
        for (int i = s.first; i < s.first + s.n; i++) {
            bool found;
            const Template* t = beat_tmpl[i] >= 0 ? &tm[beat_tmpl[i]] : nullptr;
            on_t[i]  = algo.snap(ctx, t, beat_k[i], beats[i], &found);
            on_ok[i] = found;
        }
    };
    for (Segment& s : segs) {
        if (s.n <= 0) continue;
        snap_all(s);
        std::vector<double> x(s.n);
        for (int i = 0; i < s.n; i++) {
            int bi = s.first + i;
            // Grown segments use their own (already tempo-tracked) spacing as
            // the abscissa, so the least-squares refit re-centres on onsets
            // without fighting the local tempo the growth followed.
            x[i] = (s.tmpl >= 0 && beat_k[bi] >= 0) ? tm[s.tmpl].off[beat_k[bi]]
                 : (s.tmpl == -2)                   ? beats[bi] - beats[s.first]
                                                    : (double)i * ctx.period;
        }
        int nf = 0; double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < s.n; i++) {
            if (!on_ok[s.first + i]) continue;
            double yv = on_t[s.first + i];
            sx += x[i]; sy += yv; sxx += x[i] * x[i]; sxy += x[i] * yv; nf++;
        }
        if (p.refit && nf >= 4 && s.n >= 4) {
            double den = nf * sxx - sx * sx;
            if (fabs(den) > 1e-9) {
                double r  = (nf * sxy - sx * sy) / den;     // seconds per unit x
                double r0 = s.tmpl >= 0 ? s.r : 1.0;        // what the placement assumed
                double lo = r0 * (1.0 - 2.0 * p.max_warp), hi = r0 * (1.0 + 2.0 * p.max_warp);
                if (r < lo) r = lo;
                if (r > hi) r = hi;
                double a = (sy - r * sx) / nf;
                for (int i = 0; i < s.n; i++) beats[s.first + i] = a + r * x[i];
                s.r = r;
                snap_all(s);   // onsets relative to the refitted positions
            }
        }
        for (int i = s.first; i < s.first + s.n; i++) {
            if (on_ok[i]) {
                double dist = fabs(on_t[i] - beats[i]) / std::max(1e-6, p.onset_window * ctx.period);
                beats[i] += p.onset_weight * (on_t[i] - beats[i]);
                conf[i] = 1.0f - 0.5f * (float)std::min(1.0, dist);
            } else {
                conf[i] = 0.35f;
            }
        }
    }
    for (size_t i = 1; i < beats.size(); i++)
        if (beats[i] <= beats[i - 1]) beats[i] = beats[i - 1] + 1e-3;

    // Light smoothing with the anchors pinned
    {
        std::vector<double> arr;
        if (g.has_l) arr.push_back(g.t0);
        arr.insert(arr.end(), beats.begin(), beats.end());
        if (g.has_r) arr.push_back(g.t1);
        if (arr.size() >= 3)
            beat_smooth_times(arr.data(), (int)arr.size(), &p.smooth,
                              ctx.onsets.empty() ? nullptr : ctx.onsets.data(), (int)ctx.onsets.size());
        size_t o = g.has_l ? 1 : 0;
        for (size_t i = 0; i < beats.size(); i++) beats[i] = arr[o + i];
    }
    for (size_t i = 1; i < beats.size(); i++)
        if (beats[i] <= beats[i - 1]) beats[i] = beats[i - 1] + 1e-3;

    // Emit candidates
    for (const Segment& s : segs) {
        if (s.n <= 0) continue;
        CompleteCand c = {};
        c.kind  = CAND_BEATS;
        c.first = (int)out->beat_times.size() + s.first;
        c.n     = s.n;
        double tl = s.first > 0 ? beats[s.first - 1] : (g.has_l ? g.t0 : beats[s.first]);
        c.t0 = tl;
        c.t1 = beats[s.first + s.n - 1];
        c.selected = true;
        char a[16], b[16];
        fmt_time(a, sizeof(a), c.t0); fmt_time(b, sizeof(b), c.t1);
        if (s.tmpl >= 0) {
            const Template& t = tm[s.tmpl];
            double lw = 0; int ln = 0;
            for (int i = 1; i < s.n; i++) {
                double a0 = beats[s.first + i] - beats[s.first + i - 1];
                double b0 = before[s.first + i] - before[s.first + i - 1];
                if (a0 > 1e-6 && b0 > 1e-6) { lw += fabs(log(a0 / b0)); ln++; }
            }
            c.warp  = (float)fabs(s.r - 1.0) + (ln ? (float)(lw / ln) : 0.0f);
            c.sim   = s.sim;
            c.score = std::max(0.0f, std::min(1.0f, s.score));
            strncpy(c.source, t.name, sizeof(c.source) - 1);
            if (ai >= 1)
                snprintf(c.desc, sizeof(c.desc), "%d beats from \"%s\" x%.3f  %s-%s  rhythm %.2f chroma %.2f warp %.1f%%",
                         s.n, t.name, s.r, a, b, s.rhythm, s.sim, 100.0 * c.warp);
            else
                snprintf(c.desc, sizeof(c.desc), "%d beats from \"%s\" x%.3f  %s-%s  sim %.2f warp %.1f%%",
                         s.n, t.name, s.r, a, b, s.sim, 100.0 * c.warp);
        } else if (s.tmpl == -2) {
            double bpm = s.n > 0 ? 60.0 * s.n / std::max(1e-6, c.t1 - tl) : 0.0;
            c.warp  = 0; c.sim = 0;
            float cs = 0; for (int i = 0; i < s.n; i++) cs += conf[s.first + i];
            c.score = s.n ? 0.85f * cs / s.n : 0.4f;
            strncpy(c.source, "region-grow", sizeof(c.source) - 1);
            snprintf(c.desc, sizeof(c.desc), "%d beats grown from anchor ~%.0f BPM  %s-%s",
                     s.n, bpm, a, b);
        } else {
            double bpm = s.n > 0 ? 60.0 * s.n / std::max(1e-6, c.t1 - tl) : 0.0;
            c.warp  = 0; c.sim = 0;
            float cs = 0; for (int i = 0; i < s.n; i++) cs += conf[s.first + i];
            c.score = s.n ? 0.8f * cs / s.n : 0.4f;
            if (s.seam && c.score > 0.35f) c.score = 0.35f;
            strncpy(c.source, s.seam ? "bridge (grid break?)" : "tempo", sizeof(c.source) - 1);
            snprintf(c.desc, sizeof(c.desc), "%d beats, %s ~%.0f BPM  %s-%s%s",
                     s.n, ai == 2 ? "bridge" : "tempo fill", bpm, a, b,
                     s.seam ? "  [grid break?]" : "");
        }
        out->cands.push_back(c);
    }
    out->beat_times.insert(out->beat_times.end(), beats.begin(), beats.end());
    out->beat_conf.insert(out->beat_conf.end(), conf.begin(), conf.end());
    out->onsets.insert(out->onsets.end(), ctx.onsets.begin(), ctx.onsets.end());
}

// ===========================================================================
// Second-stage smoothing, per segment
// ===========================================================================

int complete_smooth_segments(CompleteProposal* prop, const BeatMap* bm,
                             const SmoothParams& sp, bool selected_only)
{
    std::sort(prop->onsets.begin(), prop->onsets.end());
    int done = 0;
    for (CompleteCand& c : prop->cands) {
        if (c.kind != CAND_BEATS || c.n < 1) continue;
        if (selected_only && !c.selected) continue;
        if (c.first < 0 || c.first + c.n > (int)prop->beat_times.size()) continue;

        double* seg = &prop->beat_times[c.first];
        double period = c.n > 1 ? (seg[c.n - 1] - seg[0]) / (c.n - 1) : 0.5;
        if (c.n == 1) {
            // Use the distance to the previous beat as the period
            period = seg[0] - c.t0 > 1e-3 ? seg[0] - c.t0 : 0.5;
        }

        // Left pin: the beat this segment continues from (candidate t0 is that
        // beat, or the segment's own first beat when there is none).
        bool has_l = c.t0 < seg[0] - 1e-6;
        double tl = c.t0;
        // Right pin: the next proposed beat or the next mapped beat, whichever
        // comes first, if it is within two beats.
        double tr = 1e300;
        int nxt = c.first + c.n;
        if (nxt < (int)prop->beat_times.size()) tr = prop->beat_times[nxt];
        int mi = first_beat_at(bm, seg[c.n - 1] + 1e-6);
        if (mi < bm->count && bm->beats[mi].time < tr) tr = bm->beats[mi].time;
        bool has_r = tr - seg[c.n - 1] < 2.0 * period;

        std::vector<double> arr;
        if (has_l) arr.push_back(tl);
        arr.insert(arr.end(), seg, seg + c.n);
        if (has_r) arr.push_back(tr);
        if (arr.size() < 3) continue;

        beat_smooth_times(arr.data(), (int)arr.size(), &sp,
                          prop->onsets.empty() ? nullptr : prop->onsets.data(),
                          (int)prop->onsets.size());
        size_t o = has_l ? 1 : 0;
        for (int i = 0; i < c.n; i++) seg[i] = arr[o + i];
        for (int i = 1; i < c.n; i++) if (seg[i] <= seg[i - 1]) seg[i] = seg[i - 1] + 1e-3;
        c.t1 = seg[c.n - 1];
        done++;
    }
    return done;
}

// ===========================================================================
// Beat features: chroma + rhythm per mapped beat interval
// ===========================================================================
//
// The rhythm map (which onset shape hits on which sub-beat slot) is as useful
// for "is this the same passage?" as chroma is, and it is what tells a verse
// from a chorus when the harmony is the same.  Every range comparison below
// blends the two.

struct BeatFeatures {
    int n = 0;               // intervals
    int rdim = 0;            // rhythm vector length (slots * K), 0 = no vocabulary
    std::vector<float> rhythm;   // n * rdim, presence-weighted shape distribution per slot
    const BeatChromaCache* chroma = nullptr;
};

static void build_beat_features(const BeatMap* bm, const BeatChromaCache* cache,
                                const ShapeAnalysis* sa, int slots, BeatFeatures* f)
{
    f->n = bm->count > 0 ? bm->count - 1 : 0;
    f->chroma = cache;
    f->rdim = 0;
    f->rhythm.clear();
    if (!sa || !sa->vocab.valid || slots < 1 || f->n <= 0) return;
    int K = sa->vocab.k;
    f->rdim = slots * K;
    f->rhythm.assign((size_t)f->n * f->rdim, 0.0f);
    for (int i = 0; i < f->n; i++) {
        double a = bm->beats[i].time, b = bm->beats[i + 1].time;
        if (b <= a) continue;
        int ia = shape_onset_at(*sa, a), ib = shape_onset_at(*sa, b);
        float* v = &f->rhythm[(size_t)i * f->rdim];
        for (int o = ia; o < ib; o++) {
            int s = (int)((sa->onset_t[o] - a) / (b - a) * slots);
            if (s < 0) s = 0; if (s >= slots) s = slots - 1;
            // Loudness-weighted: the window's level relative to -40 dB, floored
            float pres = 0.3f + 0.7f * (sa->onset_energy[o] + 40.0f) / 40.0f;
            if (pres < 0.3f) pres = 0.3f; if (pres > 1.0f) pres = 1.0f;
            for (int c = 0; c < K; c++) {
                float x = pres * sa->onset_soft[(size_t)o * K + c];
                if (x > v[s * K + c]) v[s * K + c] = x;
            }
        }
    }
}

static float vec_cosine(const float* a, const float* b, int n) {
    float dot = 0, na = 0, nb = 0;
    for (int i = 0; i < n; i++) { dot += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; }
    if (na < 1e-12f || nb < 1e-12f) return 0.0f;
    return dot / sqrtf(na * nb);
}

// Average chroma of cache intervals [i0, i1) into out.
static void avg_chroma(const BeatChromaCache* c, int i0, int i1, float out[12]) {
    memset(out, 0, sizeof(float) * 12);
    int n = 0;
    for (int i = i0; i < i1 && i < (int)c->entries.size(); i++, n++)
        for (int k = 0; k < 12; k++) out[k] += c->entries[i].v[k];
    if (n) for (int k = 0; k < 12; k++) out[k] /= n;
}

static void avg_rhythm(const BeatFeatures& f, int i0, int i1, std::vector<float>* out) {
    out->assign(f.rdim, 0.0f);
    int n = 0;
    for (int i = i0; i < i1 && i < f.n; i++, n++)
        for (int k = 0; k < f.rdim; k++) (*out)[k] += f.rhythm[(size_t)i * f.rdim + k];
    if (n) for (int k = 0; k < f.rdim; k++) (*out)[k] /= n;
}

// Similarity of beats [a, a+n) vs [b, b+n), compared `group` beats at a time
// (a measure, or one beat), blending chroma and rhythm by rw.
static float range_similarity(const BeatFeatures& f, int a, int b, int n, int group, float rw) {
    if (group < 1) group = 1;
    if (f.rdim == 0) rw = 0.0f;
    float sum = 0; int cnt = 0;
    std::vector<float> ra, rb;
    for (int k = 0; k < n; k += group) {
        int g = std::min(group, n - k);
        float va[12], vb[12];
        avg_chroma(f.chroma, a + k, a + k + g, va);
        avg_chroma(f.chroma, b + k, b + k + g, vb);
        float s = chroma_cosine(va, vb);
        if (rw > 0.0f) {
            avg_rhythm(f, a + k, a + k + g, &ra);
            avg_rhythm(f, b + k, b + k + g, &rb);
            s = (1.0f - rw) * s + rw * vec_cosine(ra.data(), rb.data(), f.rdim);
        }
        sum += s;
        cnt++;
    }
    return cnt ? sum / cnt : 0.0f;
}

// Beats [b, b+n] all mapped with no gap inside.
static bool contiguous(const BeatMap* bm, int b, int n, double gap_thresh) {
    if (b < 0 || b + n >= bm->count) return false;
    for (int i = b + 1; i <= b + n; i++)
        if (bm->beats[i].time - bm->beats[i - 1].time > gap_thresh) return false;
    return true;
}

static void section_name(const Section& sec, char* out, int n) {
    if (sec.label[0]) snprintf(out, n, "%s %s", SECTION_KIND_NAMES[sec.kind], sec.label);
    else              snprintf(out, n, "%s", SECTION_KIND_NAMES[sec.kind]);
}

// ===========================================================================
// Section inference
// ===========================================================================

static double section_overlap(const SectionMap* sm, double t0, double t1) {
    double ov = 0;
    for (int k = 0; k < sm->count; k++)
        ov += overlap_len(t0, t1, sm->sections[k].t_start, sm->sections[k].t_end);
    return ov;
}

// Keep the best candidates that do not overlap a kept one by > 25%.  Longer
// matches go first: a 16-beat intro that is harmonically half a verse would
// otherwise tile the track in halves and pre-empt the 32-beat verse itself.
static void suppress_overlaps(std::vector<CompleteCand>* found, std::vector<CompleteCand>* kept) {
    std::sort(found->begin(), found->end(),
              [](const CompleteCand& x, const CompleteCand& y) {
                  double lx = x.t1 - x.t0, ly = y.t1 - y.t0;
                  if (fabs(lx - ly) > 0.5) return lx > ly;
                  return x.score > y.score;
              });
    for (const CompleteCand& c : *found) {
        bool clash = false;
        for (const CompleteCand& k : *kept)
            if (overlap_len(c.t0, c.t1, k.t0, k.t1) > 0.25 * (c.t1 - c.t0)) { clash = true; break; }
        if (!clash) kept->push_back(c);
    }
}

// 1. Template matching: every mapped section slid across the grid.
// Spot-check the template section's chords against the candidate placement:
// each chord's span at the same beat offsets should sound like it does in
// the template.  When they do, the chords ride along with the section so the
// two are accepted as one unit.  Returns the mean similarity, or -1 when the
// template has no chords / the chords do not fit.
static float attach_template_chords(const CompleteInputs& in, const CompleteParams& p,
                                    const BeatFeatures& f, int src_b0, int dst_b0, int n_int,
                                    CompleteProposal* out, CompleteCand* c)
{
    const BeatMap* bm = in.beatmap;
    const MiscMap* cm = in.chordmap;
    double src_t0 = bm->beats[src_b0].time, src_t1 = bm->beats[src_b0 + n_int].time;
    double dst_t1 = bm->beats[dst_b0 + n_int].time;
    double med = median_ibi(bm);
    float rw = f.rdim ? p.section_rhythm_weight : 0.0f;
    int first = (int)out->chords.size();
    float sum = 0; int cnt = 0;
    std::vector<float> ra, rb;
    for (int i = 0; i < cm->count; i++) {
        const MiscAnnotation& ch = cm->entries[i];
        if (ch.t_start < src_t0 - 0.25 * med || ch.t_start >= src_t1 - 0.25 * med) continue;
        int bs = nearest_beat(bm, ch.t_start);
        int be = nearest_beat(bm, ch.t_end);
        if (be <= bs) be = bs + 1;
        int ds = dst_b0 + (bs - src_b0), de = dst_b0 + (be - src_b0);
        if (ds < 0 || de > bm->count - 1 || ds >= dst_b0 + n_int) continue;
        float va[12], vb[12];
        avg_chroma(f.chroma, bs, be, va);
        avg_chroma(f.chroma, ds, de, vb);
        float sim = chroma_cosine(va, vb);
        if (rw > 0.0f) {
            avg_rhythm(f, bs, be, &ra); avg_rhythm(f, ds, de, &rb);
            sim = (1.0f - rw) * sim + rw * vec_cosine(ra.data(), rb.data(), f.rdim);
        }
        sum += sim * (be - bs); cnt += (be - bs);
        ChordProposal cp;
        cp.t0 = bm->beats[ds].time;
        cp.t1 = std::min(bm->beats[de].time, dst_t1);
        strncpy(cp.text, ch.text, sizeof(cp.text) - 1); cp.text[sizeof(cp.text) - 1] = 0;
        out->chords.push_back(cp);
    }
    int n = (int)out->chords.size() - first;
    if (n == 0 || cnt == 0) return -1.0f;
    float mean = sum / cnt;
    if (mean < p.chord_sim_threshold) {
        out->chords.resize(first);           // chords did not check out: section alone
        return -1.0f;
    }
    c->chord_first = first; c->chord_n = n; c->chord_sim = mean;
    return mean;
}

// 1b. Partition DP: sections tile the track, so an uncovered span between
//     known sections is not a field for independent sliding matches -- it is
//     a *sequence* of section-sized blocks that must meet the known edges
//     exactly.  A DP over the span's beats chooses that sequence: template
//     blocks (compared measure-wise, chroma + rhythm), gently truncated or
//     extended by measures (a final chorus often repeats its tail), and
//     per-beat filler for what matches nothing.  Anchoring both ends kills
//     the off-by-a-few-beats placements the sliding search was prone to.

struct SecTemplate {
    int b0, n;                 // beat index and length (intervals) in the map
    SectionKind kind;
    char label[48], name[48];
    int ts_num, ts_den;
};

// Prefix sums of the per-interval features, so any range's mean is O(dim).
struct FeatSums {
    int n = 0, rdim = 0;
    std::vector<double> pc;    // (n+1) * 12
    std::vector<double> pr;    // (n+1) * rdim
    void build(const BeatFeatures& f) {
        n = f.n; rdim = f.rdim;
        pc.assign((size_t)(n + 1) * 12, 0.0);
        pr.assign((size_t)(n + 1) * (rdim ? rdim : 1), 0.0);
        for (int i = 0; i < n; i++) {
            const float* v = i < (int)f.chroma->entries.size() ? f.chroma->entries[i].v : nullptr;
            for (int k = 0; k < 12; k++)
                pc[(size_t)(i + 1) * 12 + k] = pc[(size_t)i * 12 + k] + (v ? v[k] : 0.0f);
            if (rdim)
                for (int k = 0; k < rdim; k++)
                    pr[(size_t)(i + 1) * rdim + k] = pr[(size_t)i * rdim + k] + f.rhythm[(size_t)i * rdim + k];
        }
    }
    // Cosine of the mean feature over [a0,a1) vs [b0,b1), chroma+rhythm blended.
    float cos_range(int a0, int a1, int b0, int b1, float rw) const {
        double dot = 0, na = 0, nb = 0;
        for (int k = 0; k < 12; k++) {
            double x = (pc[(size_t)a1 * 12 + k] - pc[(size_t)a0 * 12 + k]) / (a1 - a0);
            double y = (pc[(size_t)b1 * 12 + k] - pc[(size_t)b0 * 12 + k]) / (b1 - b0);
            dot += x * y; na += x * x; nb += y * y;
        }
        float cs = (na > 1e-12 && nb > 1e-12) ? (float)(dot / sqrt(na * nb)) : 0.0f;
        if (!rdim || rw <= 0.0f) return cs;
        dot = na = nb = 0;
        for (int k = 0; k < rdim; k++) {
            double x = (pr[(size_t)a1 * rdim + k] - pr[(size_t)a0 * rdim + k]) / (a1 - a0);
            double y = (pr[(size_t)b1 * rdim + k] - pr[(size_t)b0 * rdim + k]) / (b1 - b0);
            dot += x * y; na += x * x; nb += y * y;
        }
        float cr = (na > 1e-12 && nb > 1e-12) ? (float)(dot / sqrt(na * nb)) : 0.0f;
        return (1.0f - rw) * cs + rw * cr;
    }
};

// Similarity of template t placed at beat x with block length len (intervals):
// measure-by-measure; extra measures beyond the template compare cyclically
// against its last two measures (a tail that repeats).
static float block_sim(const FeatSums& fs, const SecTemplate& t, int x, int len,
                       int ts, float rw)
{
    float sum = 0; int cnt = 0;
    for (int m = 0; m * ts < len; m++) {
        int d0 = x + m * ts, d1 = std::min(x + (m + 1) * ts, x + len);
        int tm = m;
        if (tm * ts >= t.n) {                    // extension: cycle the last two measures
            int tail = std::max(1, std::min(2, t.n / ts));
            int first_tail = t.n / ts - tail;
            tm = first_tail + (tm - t.n / ts) % tail;
        }
        int s0 = t.b0 + tm * ts, s1 = std::min(t.b0 + (tm + 1) * ts, t.b0 + t.n);
        if (s1 <= s0 || d1 <= d0) continue;
        sum += fs.cos_range(s0, s1, d0, d1, rw);
        cnt++;
    }
    return cnt ? sum / cnt : 0.0f;
}

// Fill the uncovered beat span [s0, s1] (indices into the map, s1 exclusive as
// an interval end) with the best sequence of template blocks and filler.
// open_left / open_right: that edge is the track boundary, not a known
// section, so a block need not meet it exactly (filler absorbs the rest).
// Bigram prior over section kinds, learned from the sections already in the
// map: this song's verses are followed by choruses (or solos), and the DP
// should prefer sequences the song itself uses.  Distinguishes a solo from a
// verse over the same twelve bars, which chroma alone cannot.
struct KindPrior {
    int seen[SK_COUNT + 1][SK_COUNT + 1] = {};   // index SK_COUNT = span edge / unknown
    void build(const SectionMap* sm) {
        std::vector<std::pair<double, int>> seq;
        for (int i = 0; i < sm->count; i++) seq.push_back({ sm->sections[i].t_start, sm->sections[i].kind });
        std::sort(seq.begin(), seq.end());
        for (size_t i = 0; i + 1 < seq.size(); i++)
            seen[seq[i].second][seq[i + 1].second]++;
    }
    float bonus(int a, int b) const {
        if (a > SK_COUNT || b > SK_COUNT || a < 0 || b < 0) return 0.0f;
        return seen[a][b] > 0 ? 1.0f : -0.5f;
    }
};

static void partition_span(const CompleteInputs& in, const CompleteParams& p,
                           const BeatFeatures& f, const FeatSums& fs,
                           const std::vector<SecTemplate>& tm, int ts,
                           int s0, int s1, int kind_l, int kind_r,
                           const KindPrior& prior, CompleteProposal* out,
                           std::vector<CompleteCand>* kept)
{
    const BeatMap* bm = in.beatmap;
    int B = s1 - s0;
    if (B < 2) return;
    float rw = p.section_rhythm_weight;
    const float FILL = 0.45f;                       // per-beat score of "no section here"
    const float EXT_PEN = 0.01f, TRUNC_PEN = 0.04f; // per measure
    const float PRIOR_W = p.section_prior_weight;   // score units per transition
    const float BLOCK_PEN = p.section_block_penalty; // fixed cost per block: no confetti partitions
    float thr = p.section_sim_threshold;
    float gate = thr - 0.10f;                        // DP may consider slightly weak blocks...

    // dp state: (position, kind of the last placed block); SK_COUNT = none yet
    const int K = SK_COUNT + 1;
    struct Choice { int prev; int pk; int tmpl; int len; float sim; };
    std::vector<double> dp((size_t)(B + 1) * K, -1e18);
    std::vector<Choice> bk((size_t)(B + 1) * K, { -1, 0, -1, 0, 0 });
    int k0 = (kind_l >= 0 && kind_l < SK_COUNT) ? kind_l : SK_COUNT;
    dp[(size_t)0 * K + k0] = 0.0;
    for (int x = 0; x < B; x++) {
        for (int k = 0; k < K; k++) {
            double d = dp[(size_t)x * K + k];
            if (d < -1e17) continue;
            // filler, one beat; the last kind carries through
            size_t ni = (size_t)(x + 1) * K + k;
            if (d + FILL > dp[ni]) { dp[ni] = d + FILL; bk[ni] = { x, k, -1, 1, 0 }; }
            for (size_t ti = 0; ti < tm.size(); ti++) {
                const SecTemplate& t = tm[ti];
                int down = (int)(0.4 * t.n / ts);
                int min_len = std::max(2 * ts, t.n - down * ts);
                int max_len = std::max(t.n + 8 * ts, (int)(3.5 * t.n) / ts * ts);
                for (int len = min_len; len <= max_len; len += ts) {
                    if (x + len > B) break;
                    float sim = block_sim(fs, t, s0 + x, len, ts, rw);
                    float pen = 0.0f;
                    if (len < t.n) pen = TRUNC_PEN * (float)(t.n - len) / ts;
                    if (len > t.n) pen = EXT_PEN * (float)(len - t.n) / ts;
                    float eff = sim - pen;
                    if (eff < gate) continue;
                    double sc = d + (double)eff * len - BLOCK_PEN + PRIOR_W * prior.bonus(k, t.kind);
                    // Meeting the right edge earns the closing transition too
                    if (x + len == B && kind_r >= 0)
                        sc += PRIOR_W * prior.bonus(t.kind, kind_r);
                    size_t bi = (size_t)(x + len) * K + t.kind;
                    if (sc > dp[bi]) { dp[bi] = sc; bk[bi] = { x, k, (int)ti, len, sim }; }
                }
            }
        }
    }
    // Backtrack from the best end state
    int bx = B, bkind = 0; double best = -1e18;
    for (int k = 0; k < K; k++)
        if (dp[(size_t)B * K + k] > best) { best = dp[(size_t)B * K + k]; bkind = k; }
    if (best < -1e17) return;
    std::vector<Choice> path;
    int x = bx, k = bkind;
    while (x > 0) {
        Choice c = bk[(size_t)x * K + k];
        if (c.prev < 0) break;
        path.push_back(c);
        x = c.prev; k = c.pk;
    }
    std::reverse(path.begin(), path.end());
    int pos = s0;
    for (const Choice& c : path) {
        if (c.tmpl >= 0) {
            const SecTemplate& t = tm[c.tmpl];
            CompleteCand cand = {};
            cand.selected = c.sim >= thr;   // ...but a weak one arrives unticked
            cand.kind = CAND_SECTION;
            cand.t0 = bm->beats[pos].time;
            cand.t1 = bm->beats[pos + c.len].time;
            cand.sim = c.sim; cand.score = c.sim;
            cand.sec_kind = t.kind; cand.ts_num = t.ts_num; cand.ts_den = t.ts_den;
            strncpy(cand.label, t.label, sizeof(cand.label) - 1);
            strncpy(cand.source, t.name, sizeof(cand.source) - 1);
            char a[16], b[16];
            fmt_time(a, sizeof(a), cand.t0); fmt_time(b, sizeof(b), cand.t1);
            int base_n = std::min(c.len, t.n);
            float cs = attach_template_chords(in, p, f, t.b0, pos, base_n, out, &cand);
            if (cs >= 0.0f)
                snprintf(cand.desc, sizeof(cand.desc), "%s  %s-%s  (%d beats%s) sim %.2f + %d chords (check %.2f)",
                         t.name, a, b, c.len, c.len == t.n ? "" : c.len > t.n ? ", extended" : ", short",
                         c.sim, cand.chord_n, cs);
            else
                snprintf(cand.desc, sizeof(cand.desc), "%s  %s-%s  (%d beats%s) sim %.2f",
                         t.name, a, b, c.len, c.len == t.n ? "" : c.len > t.n ? ", extended" : ", short",
                         c.sim);
            kept->push_back(cand);
        }
        pos += c.len;
    }
}

static void sections_partition(const CompleteInputs& in, const CompleteParams& p,
                               const BeatFeatures& f, double gap_thresh,
                               CompleteProposal* out, std::vector<CompleteCand>* kept)
{
    const BeatMap* bm = in.beatmap;
    const SectionMap* sm = in.sectionmap;
    if (bm->count < 4 || sm->count == 0) return;

    // Templates: every fully mapped section
    std::vector<SecTemplate> tm;
    int ts = 4;
    for (int s = 0; s < sm->count; s++) {
        const Section& sec = sm->sections[s];
        int b0 = nearest_beat(bm, sec.t_start);
        int b1 = nearest_beat(bm, sec.t_end);
        int n = b1 - b0;
        if (n < 2 || !contiguous(bm, b0, n, gap_thresh)) continue;
        SecTemplate t;
        t.b0 = b0; t.n = n; t.kind = sec.kind;
        t.ts_num = sec.ts_num > 0 ? sec.ts_num : 4;
        t.ts_den = sec.ts_den > 0 ? sec.ts_den : 4;
        ts = t.ts_num;
        strncpy(t.label, sec.label, sizeof(t.label) - 1); t.label[sizeof(t.label) - 1] = 0;
        section_name(sec, t.name, sizeof(t.name));
        tm.push_back(t);
    }
    if (tm.empty()) return;
    // An anomalously short section (a 4-second "verse" tag) makes a template
    // that can tile anything in confetti; templates far below the median
    // length sit out of the DP.
    {
        std::vector<int> lens;
        for (const SecTemplate& t : tm) lens.push_back(t.n);
        std::sort(lens.begin(), lens.end());
        int med = lens[lens.size() / 2];
        std::vector<SecTemplate> keep;
        for (const SecTemplate& t : tm) if (t.n >= (int)(0.35 * med)) keep.push_back(t);
        if (!keep.empty()) tm.swap(keep);
    }

    FeatSums fs;
    fs.build(f);

    // Uncovered spans between known sections (and the track edges), as beat
    // index ranges over contiguous beats.
    struct Edge { double t; };
    std::vector<std::pair<double, double>> covered;
    for (int s = 0; s < sm->count; s++)
        covered.push_back({ sm->sections[s].t_start, sm->sections[s].t_end });
    std::sort(covered.begin(), covered.end());
    std::vector<std::pair<double, double>> spans;
    double cur = 0.0;
    for (auto& cv : covered) {
        if (cv.first - cur > 0.5) spans.push_back({ cur, cv.first });
        cur = std::max(cur, cv.second);
    }
    if (in.duration - cur > 0.5) spans.push_back({ cur, in.duration });

    KindPrior prior;
    prior.build(sm);
    for (auto& sp : spans) {
        // Kinds of the known sections either side of the span, for the prior
        int kind_l = -1, kind_r = -1;
        for (int q = 0; q < sm->count; q++) {
            if (fabs(sm->sections[q].t_end - sp.first) < 0.3)   kind_l = sm->sections[q].kind;
            if (fabs(sm->sections[q].t_start - sp.second) < 0.3) kind_r = sm->sections[q].kind;
        }
        int s0 = nearest_beat(bm, sp.first);
        int s1 = nearest_beat(bm, sp.second);
        if (fabs(bm->beats[s0].time - sp.first) > 1.0) {          // span edge off the grid
            // open span at the track edge: clamp to the first/last beat
            if (sp.first < bm->beats[0].time) s0 = 0; else continue;
        }
        if (fabs(bm->beats[s1].time - sp.second) > 1.0) {
            if (sp.second > bm->beats[bm->count - 1].time) s1 = bm->count - 1; else continue;
        }
        if (s1 - s0 < 2 || !contiguous(bm, s0, s1 - s0, gap_thresh)) continue;
        partition_span(in, p, f, fs, tm, ts, s0, s1, kind_l, kind_r, prior, out, kept);
    }
}

static void sections_from_templates(const CompleteInputs& in, const CompleteParams& p,
                                    const BeatFeatures& f, double gap_thresh,
                                    CompleteProposal* out, std::vector<CompleteCand>* kept)
{
    const BeatMap* bm = in.beatmap;
    const SectionMap* sm = in.sectionmap;
    std::vector<CompleteCand> found;
    for (int s = 0; s < sm->count; s++) {
        const Section& sec = sm->sections[s];
        int b0 = nearest_beat(bm, sec.t_start);
        int b1 = nearest_beat(bm, sec.t_end);
        int n_int = b1 - b0;
        if (n_int < 2 || !contiguous(bm, b0, n_int, gap_thresh)) continue;
        int group = p.section_granularity == 0 ? std::max(1, sec.ts_num) : 1;
        char src[48]; section_name(sec, src, sizeof(src));

        for (int b = 0; b + n_int < bm->count; b++) {
            if (b == b0 || !contiguous(bm, b, n_int, gap_thresh)) continue;
            double t0 = bm->beats[b].time, t1 = bm->beats[b + n_int].time;
            if (section_overlap(sm, t0, t1) > p.section_max_overlap * (t1 - t0)) continue;
            float sim = range_similarity(f, b0, b, n_int, group, p.section_rhythm_weight);
            if (sim < p.section_sim_threshold) continue;
            int bp = b;   // the placement, possibly refined below
            // Only keep a local maximum over +-1 beat, then refine the exact
            // beat at beat granularity: measure averages hardly move under a
            // one-beat shift, beat-level chroma and the rhythm map do.
            {
                float l = b > 0 && contiguous(bm, b - 1, n_int, gap_thresh)
                        ? range_similarity(f, b0, b - 1, n_int, group, p.section_rhythm_weight) : -1.0f;
                float r = b + 1 + n_int < bm->count && contiguous(bm, b + 1, n_int, gap_thresh)
                        ? range_similarity(f, b0, b + 1, n_int, group, p.section_rhythm_weight) : -1.0f;
                if (l > sim || r > sim) continue;
                int best_b = b; float best_fine = -1.0f;
                for (int d = -2; d <= 2; d++) {
                    int bb = b + d;
                    if (bb < 0 || bb == b0 || bb + n_int >= bm->count || !contiguous(bm, bb, n_int, gap_thresh)) continue;
                    float fine = range_similarity(f, b0, bb, n_int, 1, std::max(p.section_rhythm_weight, 0.5f));
                    if (fine > best_fine) { best_fine = fine; best_b = bb; }
                }
                if (best_b != b) {
                    bp = best_b;
                    t0 = bm->beats[bp].time; t1 = bm->beats[bp + n_int].time;
                    if (section_overlap(sm, t0, t1) > p.section_max_overlap * (t1 - t0)) continue;
                    sim = range_similarity(f, b0, bp, n_int, group, p.section_rhythm_weight);
                }
            }
            CompleteCand c = {};
            c.kind = CAND_SECTION; c.t0 = t0; c.t1 = t1;
            c.sim = sim; c.score = sim; c.selected = true;
            c.sec_kind = sec.kind; c.ts_num = sec.ts_num; c.ts_den = sec.ts_den;
            strncpy(c.label, sec.label, sizeof(c.label) - 1);
            strncpy(c.source, src, sizeof(c.source) - 1);
            char a[16], bb[16];
            fmt_time(a, sizeof(a), t0); fmt_time(bb, sizeof(bb), t1);
            float cs = attach_template_chords(in, p, f, b0, bp, n_int, out, &c);
            if (cs >= 0.0f)
                snprintf(c.desc, sizeof(c.desc), "%s  %s-%s  (%d beats) sim %.2f + %d chords (check %.2f)",
                         src, a, bb, n_int, sim, c.chord_n, cs);
            else
                snprintf(c.desc, sizeof(c.desc), "%s  %s-%s  (%d beats) sim %.2f", src, a, bb, n_int, sim);
            found.push_back(c);
        }
    }
    suppress_overlaps(&found, kept);
}

// 2. Discovery: repeated blocks found by self-similarity, with no template.
//    A block is a section unit when it is immediately followed by its own
//    repeat (the smallest consecutive period, scanned shortest-first from
//    section_min_measures); a second pass attaches non-consecutive echoes of
//    each group.  Groups come out as A, B, C; the most repeated is called the
//    chorus, the rest verses, for the user to rename.
static void sections_from_repeats(const CompleteInputs& in, const CompleteParams& p,
                                  const BeatFeatures& f, double gap_thresh,
                                  std::vector<CompleteCand>* kept)
{
    const BeatMap* bm = in.beatmap;
    const SectionMap* sm = in.sectionmap;
    int ts = 4;
    for (int s = 0; s < sm->count; s++) if (sm->sections[s].ts_num > 0) { ts = sm->sections[s].ts_num; break; }
    int group = p.section_granularity == 0 ? ts : 1;
    int n_int = bm->count - 1;
    int min_m = p.section_min_measures < 1 ? 1 : p.section_min_measures;
    if (n_int < 2 * min_m * ts) return;
    float rw = p.section_rhythm_weight;

    int anchor = 0;
    if (sm->count > 0) anchor = nearest_beat(bm, sm->sections[0].t_start) % ts;

    struct Block { int b, len, grp; float sim; };
    std::vector<Block> blocks;
    auto covered = [&](int b, int len) {
        if (b < 0 || b + len > n_int) return true;
        double t0 = bm->beats[b].time, t1 = bm->beats[b + len].time;
        if (section_overlap(sm, t0, t1) > p.section_max_overlap * (t1 - t0)) return true;
        for (const CompleteCand& k : *kept)
            if (overlap_len(t0, t1, k.t0, k.t1) > 0.25 * (t1 - t0)) return true;
        for (const Block& bl : blocks) {
            double u0 = bm->beats[bl.b].time, u1 = bm->beats[bl.b + bl.len].time;
            if (overlap_len(t0, t1, u0, u1) > 0.25 * (t1 - t0)) return true;
        }
        return false;
    };
    auto usable = [&](int b, int len) { return !covered(b, len) && contiguous(bm, b, len, gap_thresh); };

    static const int LENGTHS_M[] = { 2, 3, 4, 6, 8, 12, 16, 24, 32 };
    const int NL = (int)(sizeof(LENGTHS_M) / sizeof(LENGTHS_M[0]));
    int next_grp = 0;
    std::vector<int> grp_len;            // unit length per group

    // Best similarity of [b, b+len) against any block of group g
    auto group_sim = [&](int g, int b, int len) {
        float best = -1.0f;
        for (const Block& bl : blocks)
            if (bl.grp == g && bl.len == len)
                best = std::max(best, range_similarity(f, bl.b, b, len, group, rw));
        return best;
    };

    for (int i = anchor; i + min_m * ts <= n_int; i += ts) {
        if (covered(i, ts)) continue;
        // (a) an existing unit recurring here?  Longest unit first, so a
        //     12-bar verse is not pre-empted by a 4-bar riff inside it.
        int best_g = -1; float best_s = -1.0f;
        for (int g = 0; g < next_grp; g++) {
            int L = grp_len[g];
            if (!usable(i, L)) continue;
            float sj = group_sim(g, i, L);
            if (sj >= p.section_sim_threshold &&
                (best_g < 0 || L > grp_len[best_g] || (L == grp_len[best_g] && sj > best_s))) {
                best_g = g; best_s = sj;
            }
        }
        if (best_g >= 0) { blocks.push_back({ i, grp_len[best_g], best_g, best_s }); continue; }
        // (b) a new unit: the shortest length that repeats back to back
        for (int li = 0; li < NL; li++) {
            int L = LENGTHS_M[li] * ts;
            if (LENGTHS_M[li] < min_m) continue;
            if (i + 2 * L > n_int) break;
            if (!usable(i, L) || !usable(i + L, L)) continue;
            float sim = range_similarity(f, i, i + L, L, group, rw);
            if (sim < p.section_sim_threshold) continue;
            int g = next_grp++;
            grp_len.push_back(L);
            blocks.push_back({ i, L, g, sim });
            blocks.push_back({ i + L, L, g, sim });
            break;
        }
    }

    // Merge groups of the same length whose blocks are mutually similar
    std::vector<int> alias(next_grp);
    for (int g = 0; g < next_grp; g++) alias[g] = g;
    for (int g = 0; g < next_grp; g++) {
        if (alias[g] != g) continue;
        for (int h = g + 1; h < next_grp; h++) {
            if (alias[h] != h || grp_len[h] != grp_len[g]) continue;
            float best = -1.0f;
            for (const Block& bl : blocks)
                if (bl.grp == h) best = std::max(best, group_sim(g, bl.b, bl.len));
            if (best >= p.section_sim_threshold) alias[h] = g;
        }
    }
    for (Block& bl : blocks) bl.grp = alias[bl.grp];
    // Renumber groups by first appearance so labels read A, B, C in time order
    {
        std::vector<int> order(next_grp, -1);
        int n = 0;
        std::vector<Block> sorted = blocks;
        std::sort(sorted.begin(), sorted.end(), [](const Block& x, const Block& y) { return x.b < y.b; });
        for (const Block& bl : sorted) if (order[bl.grp] < 0) order[bl.grp] = n++;
        for (Block& bl : blocks) bl.grp = order[bl.grp];
        next_grp = n;
    }
    if (blocks.empty()) return;

    std::vector<int> size(next_grp, 0);
    for (const Block& bl : blocks) size[bl.grp]++;
    int chorus = 0;
    for (int g = 1; g < next_grp; g++) if (size[g] > size[chorus]) chorus = g;
    bool name_chorus = next_grp >= 2 && size[chorus] >= 3;

    for (const Block& bl : blocks) {
        double t0 = bm->beats[bl.b].time, t1 = bm->beats[bl.b + bl.len].time;
        CompleteCand c = {};
        c.kind = CAND_SECTION; c.t0 = t0; c.t1 = t1;
        c.sim = bl.sim; c.score = bl.sim * 0.95f;    // discovered: just below a template hit
        c.selected = true;
        c.sec_kind = (name_chorus && bl.grp == chorus) ? SK_CHORUS : SK_VERSE;
        c.ts_num = ts; c.ts_den = 4;
        snprintf(c.label, sizeof(c.label), "%c", 'A' + (bl.grp % 26));
        snprintf(c.source, sizeof(c.source), "repeat %c x%d", 'A' + (bl.grp % 26), size[bl.grp]);
        char a[16], bb[16];
        fmt_time(a, sizeof(a), t0); fmt_time(bb, sizeof(bb), t1);
        snprintf(c.desc, sizeof(c.desc), "repeat %c (x%d)  %s-%s  (%d measures) sim %.2f",
                 'A' + (bl.grp % 26), size[bl.grp], a, bb, bl.len / ts, bl.sim);
        kept->push_back(c);
    }
}

static void infer_sections(const CompleteInputs& in, const CompleteParams& p,
                           const BeatFeatures& f, double gap_thresh,
                           CompleteProposal* out)
{
    if (in.beatmap->count < 4) return;
    std::vector<CompleteCand> kept;
    if (p.section_partition)
        sections_partition(in, p, f, gap_thresh, out, &kept);
    else
        sections_from_templates(in, p, f, gap_thresh, out, &kept);
    if (p.section_discover) sections_from_repeats(in, p, f, gap_thresh, &kept);
    out->cands.insert(out->cands.end(), kept.begin(), kept.end());
}

// ===========================================================================
// Chord inference
// ===========================================================================

static int chords_in(const MiscMap* cm, double t0, double t1) {
    int n = 0;
    for (int i = 0; i < cm->count; i++)
        if (cm->entries[i].t_start >= t0 - 1e-3 && cm->entries[i].t_start < t1 - 1e-3) n++;
    return n;
}

// A run of consecutive mapped chords, as beat offsets from its first beat,
// with the chroma/rhythm each chord has in the map.
struct ChordRun {
    int   b0;                       // first beat index in the map
    int   len;                      // beats covered
    struct Entry { int off, n; const char* text; float chroma[12]; std::vector<float> rhythm; };
    std::vector<Entry> e;
    char  where[32];
};

static void build_chord_runs(const CompleteInputs& in, const CompleteParams& p,
                             const BeatFeatures& f, double gap_thresh,
                             std::vector<ChordRun>* out)
{
    out->clear();
    const BeatMap* bm = in.beatmap;
    const MiscMap* cm = in.chordmap;
    if (bm->count < 2 || cm->count == 0) return;
    double med = median_ibi(bm);
    ChordRun cur; cur.b0 = -1; cur.len = 0;
    auto flush = [&]() {
        if (cur.b0 >= 0 && (int)cur.e.size() >= 2 && cur.len >= 2) {
            char a[16], b[16];
            fmt_time(a, sizeof(a), bm->beats[cur.b0].time);
            fmt_time(b, sizeof(b), bm->beats[std::min(bm->count - 1, cur.b0 + cur.len)].time);
            snprintf(cur.where, sizeof(cur.where), "%s-%s", a, b);
            out->push_back(cur);
        }
        cur = ChordRun(); cur.b0 = -1; cur.len = 0;
    };
    double prev_end = -1e9;
    for (int i = 0; i < cm->count; i++) {
        const MiscAnnotation& ch = cm->entries[i];
        int bs = nearest_beat(bm, ch.t_start);
        int be = nearest_beat(bm, ch.t_end);
        if (bs >= bm->count - 1 || be <= bs) { flush(); prev_end = ch.t_end; continue; }
        if (fabs(bm->beats[bs].time - ch.t_start) > 0.5 * med) { flush(); prev_end = ch.t_end; continue; }
        bool breaks = cur.b0 < 0 || ch.t_start - prev_end > 0.75 * med ||
                      !contiguous(bm, cur.b0, be - cur.b0 - 1, gap_thresh) ||
                      be - cur.b0 > p.chord_run_beats;
        if (breaks) { flush(); cur.b0 = bs; }
        ChordRun::Entry en;
        en.off = bs - cur.b0; en.n = be - bs; en.text = ch.text;
        avg_chroma(f.chroma, bs, be, en.chroma);
        if (f.rdim) avg_rhythm(f, bs, be, &en.rhythm);
        cur.e.push_back(en);
        cur.len = be - cur.b0;
        prev_end = ch.t_end;
    }
    flush();
}

// 1. Section-based transfer (highest confidence): a section without chords
//    borrows the chart of the most similar same-kind section that has one.
static void chords_from_sections(const CompleteInputs& in, const CompleteParams& p,
                                 const BeatFeatures& f, CompleteProposal* out,
                                 std::vector<CompleteCand>* kept)
{
    const BeatMap* bm = in.beatmap;
    const SectionMap* sm = in.sectionmap;
    const MiscMap* cm = in.chordmap;
    for (int d = 0; d < sm->count; d++) {
        const Section& dst = sm->sections[d];
        if (chords_in(cm, dst.t_start, dst.t_end) > 0) continue;
        int db0 = nearest_beat(bm, dst.t_start);
        int db1 = nearest_beat(bm, dst.t_end);
        int dn  = db1 - db0;
        if (dn < 1) continue;
        int best = -1; float best_sim = -1.0f;
        for (int s = 0; s < sm->count; s++) {
            if (s == d) continue;
            const Section& src = sm->sections[s];
            if (src.kind != dst.kind || chords_in(cm, src.t_start, src.t_end) == 0) continue;
            int sb0 = nearest_beat(bm, src.t_start);
            int sb1 = nearest_beat(bm, src.t_end);
            int sn  = sb1 - sb0;
            if (sn < 1) continue;
            int n = std::min(sn, dn);
            int group = p.section_granularity == 0 ? std::max(1, src.ts_num) : 1;
            float sim = range_similarity(f, sb0, db0, n, group, p.section_rhythm_weight);
            if (strcmp(src.label, dst.label) == 0) sim += 0.02f;
            if (sim > best_sim) { best_sim = sim; best = s; }
        }
        if (best < 0) continue;
        const Section& src = sm->sections[best];
        double src_bp = beatmap_beat_pos(bm, src.t_start);
        double dst_bp = beatmap_beat_pos(bm, dst.t_start);
        int first = (int)out->chords.size();
        for (int i = 0; i < cm->count; i++) {
            const MiscAnnotation& ch = cm->entries[i];
            if (ch.t_start < src.t_start - 1e-3 || ch.t_start >= src.t_end - 1e-3) continue;
            double n0 = beatmap_time_at(bm, dst_bp + beatmap_beat_pos(bm, ch.t_start) - src_bp);
            double n1 = beatmap_time_at(bm, dst_bp + beatmap_beat_pos(bm, ch.t_end)   - src_bp);
            if (n0 >= dst.t_end - 1e-3) continue;
            if (n1 > dst.t_end) n1 = dst.t_end;
            ChordProposal cp; cp.t0 = n0; cp.t1 = n1;
            strncpy(cp.text, ch.text, sizeof(cp.text) - 1); cp.text[sizeof(cp.text) - 1] = 0;
            out->chords.push_back(cp);
        }
        int n = (int)out->chords.size() - first;
        if (n <= 0) continue;
        CompleteCand c = {};
        c.kind = CAND_CHORDS; c.t0 = dst.t_start; c.t1 = dst.t_end;
        c.first = first; c.n = n;
        c.sim = std::min(1.0f, best_sim); c.score = c.sim; c.selected = true;
        char srcn[48], dstn[48];
        section_name(src, srcn, sizeof(srcn)); section_name(dst, dstn, sizeof(dstn));
        strncpy(c.source, srcn, sizeof(c.source) - 1);
        char a[16], b[16];
        fmt_time(a, sizeof(a), c.t0); fmt_time(b, sizeof(b), c.t1);
        snprintf(c.desc, sizeof(c.desc), "%d chords into %s from \"%s\"  %s-%s  sim %.2f",
                 n, dstn, srcn, a, b, c.sim);
        kept->push_back(c);
    }
}

// 2. Progression repeats: each run of mapped chords slid across the
//    chord-free grid; a placement scores by how much each chord's span sounds
//    (chroma + rhythm) like it did in the map.
static void chords_from_runs(const CompleteInputs& in, const CompleteParams& p,
                             const BeatFeatures& f, double gap_thresh,
                             CompleteProposal* out, std::vector<CompleteCand>* kept)
{
    const BeatMap* bm = in.beatmap;
    const MiscMap* cm = in.chordmap;
    std::vector<ChordRun> runs;
    build_chord_runs(in, p, f, gap_thresh, &runs);
    if (runs.empty()) return;
    float rw = f.rdim ? p.section_rhythm_weight : 0.0f;

    struct Hit { int run, b; float sim; };
    std::vector<Hit> hits;
    std::vector<float> rv;
    for (size_t ri = 0; ri < runs.size(); ri++) {
        const ChordRun& r = runs[ri];
        for (int b = 0; b + r.len < bm->count; b++) {
            if (b == r.b0 || !contiguous(bm, b, r.len, gap_thresh)) continue;
            double t0 = bm->beats[b].time, t1 = bm->beats[b + r.len].time;
            if (chords_in(cm, t0, t1) > 0) continue;
            bool clash = false;
            for (const CompleteCand& k : *kept)
                if (overlap_len(t0, t1, k.t0, k.t1) > 0.0) { clash = true; break; }
            if (clash) continue;
            float sum = 0, wsum = 0;
            for (const ChordRun::Entry& e : r.e) {
                float v[12];
                avg_chroma(f.chroma, b + e.off, b + e.off + e.n, v);
                float s = chroma_cosine(e.chroma, v);
                if (rw > 0.0f) {
                    avg_rhythm(f, b + e.off, b + e.off + e.n, &rv);
                    s = (1.0f - rw) * s + rw * vec_cosine(e.rhythm.data(), rv.data(), f.rdim);
                }
                sum += s * e.n; wsum += e.n;
            }
            float sim = wsum > 0 ? sum / wsum : 0.0f;
            if (sim >= p.chord_sim_threshold) hits.push_back({ (int)ri, b, sim });
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.sim > y.sim; });
    std::vector<CompleteCand> placed;
    for (const Hit& h : hits) {
        const ChordRun& r = runs[h.run];
        double t0 = bm->beats[h.b].time, t1 = bm->beats[h.b + r.len].time;
        bool clash = false;
        for (const CompleteCand& k : placed)
            if (overlap_len(t0, t1, k.t0, k.t1) > 0.0) { clash = true; break; }
        if (clash) continue;
        int first = (int)out->chords.size();
        for (const ChordRun::Entry& e : r.e) {
            ChordProposal cp;
            cp.t0 = bm->beats[h.b + e.off].time;
            cp.t1 = bm->beats[std::min(bm->count - 1, h.b + e.off + e.n)].time;
            strncpy(cp.text, e.text, sizeof(cp.text) - 1); cp.text[sizeof(cp.text) - 1] = 0;
            out->chords.push_back(cp);
        }
        CompleteCand c = {};
        c.kind = CAND_CHORDS; c.t0 = t0; c.t1 = t1;
        c.first = first; c.n = (int)r.e.size();
        c.sim = h.sim; c.score = h.sim * 0.97f; c.selected = true;
        snprintf(c.source, sizeof(c.source), "chords %s", r.where);
        char a[16], b[16], prog[40] = {};
        fmt_time(a, sizeof(a), t0); fmt_time(b, sizeof(b), t1);
        for (size_t i = 0; i < r.e.size() && strlen(prog) < 28; i++) {
            strncat(prog, r.e[i].text, 6);
            if (i + 1 < r.e.size()) strncat(prog, " ", 2);
        }
        if (strlen(prog) >= 28) strncat(prog, "..", 3);
        snprintf(c.desc, sizeof(c.desc), "%d chords (%s) repeat of %s  %s-%s  sim %.2f",
                 (int)r.e.size(), prog, r.where, a, b, h.sim);
        placed.push_back(c);
    }
    kept->insert(kept->end(), placed.begin(), placed.end());
}

// 3. Fallback: decode chords beat by beat where nothing else applies.
//    Emission = how much the beat's chroma resembles each chord model; the
//    models are what each chord name sounds like in this track (learned from
//    the map, blended with the textbook triad so a chord seen once is not
//    overfitted), and the vocabulary is the map's own chords first -- a song
//    with eight chords should not be offered twenty-four.  Viterbi over the
//    beats with a cost for changing chord, waived in part on measure starts,
//    gives the persistence a chord chart has.  Noisy: proposals start
//    deselected.
static const char* ROOT_NAMES[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

struct ChordModel { char text[32]; float v[12]; int n; bool seen; };

static void triad_template(int root, bool minor, float v[12]) {
    memset(v, 0, sizeof(float) * 12);
    v[root] = 1.0f; v[(root + (minor ? 3 : 4)) % 12] = 0.8f; v[(root + 7) % 12] = 0.8f;
}

// Parse a chord name's root and quality well enough to pick a triad.
static bool parse_chord(const char* text, int* root, bool* minor) {
    if (!text || !text[0]) return false;
    int r = -1;
    switch (text[0]) { case 'C': r = 0; break; case 'D': r = 2; break; case 'E': r = 4; break;
                       case 'F': r = 5; break; case 'G': r = 7; break; case 'A': r = 9; break;
                       case 'B': r = 11; break; default: return false; }
    const char* q = text + 1;
    if (*q == '#') { r = (r + 1) % 12; q++; } else if (*q == 'b') { r = (r + 11) % 12; q++; }
    *root = r;
    *minor = (q[0] == 'm' && q[1] != 'a');     // "m", "m7", "min" but not "maj"
    return true;
}

static void build_chord_models(const CompleteInputs& in, const CompleteParams& p,
                               const BeatChromaCache* cache, std::vector<ChordModel>* models)
{
    const BeatMap* bm = in.beatmap;
    const MiscMap* cm = in.chordmap;
    models->clear();
    for (int i = 0; i < cm->count; i++) {
        const MiscAnnotation& ch = cm->entries[i];
        int bs = nearest_beat(bm, ch.t_start), be = nearest_beat(bm, ch.t_end);
        if (be <= bs) continue;
        char name[32]; strncpy(name, ch.text, sizeof(name) - 1); name[sizeof(name) - 1] = 0;
        char* sp = strchr(name, ' '); if (sp) *sp = 0;          // "Em Em" -> "Em"
        ChordModel* m = nullptr;
        for (ChordModel& x : *models) if (!strcmp(x.text, name)) m = &x;
        if (!m) {
            ChordModel x; strncpy(x.text, name, sizeof(x.text) - 1); x.text[sizeof(x.text) - 1] = 0;
            memset(x.v, 0, sizeof(x.v)); x.n = 0; x.seen = true;
            models->push_back(x); m = &models->back();
        }
        for (int k = bs; k < be && k < (int)cache->entries.size(); k++) {
            for (int c = 0; c < 12; c++) m->v[c] += cache->entries[k].v[c];
            m->n++;
        }
    }
    // Blend each learned model with its triad (k pseudo-beats of it)
    for (ChordModel& m : *models) {
        int root; bool minor;
        float tri[12];
        bool have_tri = parse_chord(m.text, &root, &minor);
        float mx = 0; for (int c = 0; c < 12; c++) mx = std::max(mx, m.v[c]);
        for (int c = 0; c < 12; c++) {
            float learned = mx > 0 ? m.v[c] / mx : 0.0f;     // normalise peak to 1 like a triad
            if (have_tri) { triad_template(root, minor, tri); m.v[c] = (m.n * learned + p.chord_prior_beats * tri[c]) / (m.n + p.chord_prior_beats); }
            else m.v[c] = learned;
        }
    }
    // Unseen triads as a fallback vocabulary, penalised in the decoder
    if (p.chord_unseen_penalty < 1.0f) {
        for (int t = 0; t < 24; t++) {
            int root = t % 12; bool minor = t >= 12;
            char name[32]; snprintf(name, sizeof(name), "%s%s", ROOT_NAMES[root], minor ? "m" : "");
            bool dup = false;
            for (const ChordModel& m : *models) { int r2; bool m2; if (parse_chord(m.text, &r2, &m2) && r2 == root && m2 == minor) dup = true; }
            if (dup) continue;
            ChordModel x; strncpy(x.text, name, sizeof(x.text)); x.n = 0; x.seen = false;
            triad_template(root, minor, x.v);
            models->push_back(x);
        }
    }
}

// Viterbi decode over beat intervals [i0, i1) of the map.  out_label[i] = model
// index or -1 (beats with no chroma).
static void decode_chords(const CompleteInputs& in, const CompleteParams& p,
                          const BeatChromaCache* cache, const std::vector<ChordModel>& models,
                          int i0, int i1, int ts, int measure_anchor, std::vector<int>* out_label)
{
    int n = i1 - i0, M = (int)models.size();
    out_label->assign(n, -1);
    if (n <= 0 || M == 0) return;
    // Emissions
    std::vector<float> em((size_t)n * M);
    for (int i = 0; i < n; i++) {
        int bi = i0 + i;
        const float* v = bi < (int)cache->entries.size() ? cache->entries[bi].v : nullptr;
        for (int m = 0; m < M; m++) {
            float s = v ? chroma_cosine(v, models[m].v) : 0.0f;
            if (!models[m].seen) s -= p.chord_unseen_penalty;
            em[(size_t)i * M + m] = s;
        }
    }
    std::vector<float> score((size_t)n * M, -1e30f);
    std::vector<int>   back((size_t)n * M, -1);
    for (int m = 0; m < M; m++) score[m] = em[m];
    for (int i = 1; i < n; i++) {
        bool measure_start = ts > 0 && ((i0 + i - measure_anchor) % ts + ts) % ts == 0;
        float change = p.chord_transition * (measure_start ? (1.0f - p.chord_measure_bonus) : 1.0f);
        // Best previous state overall, and per-state stay
        int   bp = 0; float bv = -1e30f;
        for (int m = 0; m < M; m++) if (score[(size_t)(i - 1) * M + m] > bv) { bv = score[(size_t)(i - 1) * M + m]; bp = m; }
        for (int m = 0; m < M; m++) {
            float stay = score[(size_t)(i - 1) * M + m];
            float sw   = bv - change;
            if (stay >= sw) { score[(size_t)i * M + m] = stay + em[(size_t)i * M + m]; back[(size_t)i * M + m] = m; }
            else            { score[(size_t)i * M + m] = sw   + em[(size_t)i * M + m]; back[(size_t)i * M + m] = bp; }
        }
    }
    int best = 0; float bv = -1e30f;
    for (int m = 0; m < M; m++) if (score[(size_t)(n - 1) * M + m] > bv) { bv = score[(size_t)(n - 1) * M + m]; best = m; }
    for (int i = n - 1; i >= 0; i--) { (*out_label)[i] = best; best = back[(size_t)i * M + best]; if (best < 0) best = 0; }
}

static void chords_from_models(const CompleteInputs& in, const CompleteParams& p,
                               const BeatFeatures& f, CompleteProposal* out,
                               std::vector<CompleteCand>* kept)
{
    const BeatMap* bm = in.beatmap;
    const MiscMap* cm = in.chordmap;
    const SectionMap* sm = in.sectionmap;
    const BeatChromaCache* cache = f.chroma;
    if (bm->count < 2 || cache->entries.empty()) return;

    std::vector<ChordModel> models;
    build_chord_models(in, p, cache, &models);
    if (models.empty()) return;
    bool learned = false;
    for (const ChordModel& m : models) if (m.seen) learned = true;

    int ts = 4, anchor = 0;
    if (sm->count > 0) { ts = std::max(1, sm->sections[0].ts_num); anchor = nearest_beat(bm, sm->sections[0].t_start) % ts; }

    // The map says how often this song changes chord: scale the change cost
    // by the median chord length so a song that changes every two beats is
    // not forced into four-beat chords, nor the reverse.
    CompleteParams pp = p;
    if (p.chord_learn_rate && cm->count >= 3) {
        std::vector<double> lens;
        for (int i = 0; i < cm->count; i++) {
            int bs = nearest_beat(bm, cm->entries[i].t_start), be = nearest_beat(bm, cm->entries[i].t_end);
            if (be > bs) lens.push_back(be - bs);
        }
        if (!lens.empty()) {
            std::sort(lens.begin(), lens.end());
            double med = lens[lens.size() / 2];
            pp.chord_transition = p.chord_transition * (float)(med / 4.0);
        }
    }

    // Chord-free beat intervals, not reserved by a pending candidate
    int n_int = bm->count - 1;
    std::vector<char> free_(n_int, 0);
    for (int i = 0; i < n_int; i++) {
        double t0 = bm->beats[i].time, t1 = bm->beats[i + 1].time;
        if (chords_in(cm, t0, t1) > 0) continue;
        bool clash = false;
        for (const CompleteCand& k : *kept) if (overlap_len(t0, t1, k.t0, k.t1) > 0.0) { clash = true; break; }
        // Inside an annotated chord's span (chord started earlier)?
        for (int c = 0; c < cm->count && !clash; c++)
            if (cm->entries[c].t_start < t0 + 1e-3 && cm->entries[c].t_end > t0 + 1e-3) clash = true;
        free_[i] = !clash;
    }

    // Decode each free stretch; emit one candidate per stretch, split at
    // section boundaries so a whole track does not become one item.
    int i = 0;
    while (i < n_int) {
        if (!free_[i]) { i++; continue; }
        int j = i;
        while (j < n_int && free_[j]) {
            j++;
            // split at a section start (not the first beat of the stretch)
            if (j < n_int && j > i)
                for (int s = 0; s < sm->count; s++)
                    if (fabs(sm->sections[s].t_start - bm->beats[j].time) < 1e-3) { goto split; }
        }
    split:
        std::vector<int> label;
        decode_chords(in, pp, cache, models, i, j, ts, anchor, &label);
        int first = (int)out->chords.size();
        float ssum = 0; int scnt = 0;
        int k = 0;
        while (k < (int)label.size()) {
            if (label[k] < 0) { k++; continue; }
            int e = k;
            while (e < (int)label.size() && label[e] == label[k]) e++;
            ChordProposal cp;
            cp.t0 = bm->beats[i + k].time; cp.t1 = bm->beats[i + e].time;
            strncpy(cp.text, models[label[k]].text, sizeof(cp.text) - 1); cp.text[sizeof(cp.text) - 1] = 0;
            out->chords.push_back(cp);
            for (int q = k; q < e; q++) { ssum += chroma_cosine(cache->entries[i + q].v, models[label[k]].v); scnt++; }
            k = e;
        }
        int n = (int)out->chords.size() - first;
        if (n > 0) {
            CompleteCand c = {};
            c.kind = CAND_CHORDS; c.t0 = bm->beats[i].time; c.t1 = bm->beats[j].time;
            c.first = first; c.n = n;
            c.sim = scnt ? ssum / scnt : 0.0f; c.score = c.sim * 0.9f;
            c.selected = false;
            strncpy(c.source, learned ? "learned chords" : "triads", sizeof(c.source) - 1);
            char a[16], b[16];
            fmt_time(a, sizeof(a), c.t0); fmt_time(b, sizeof(b), c.t1);
            snprintf(c.desc, sizeof(c.desc), "%d chords decoded  %s-%s  (%d beats, %s) fit %.2f",
                     n, a, b, j - i, learned ? "learned models" : "triads", c.sim);
            kept->push_back(c);
        }
        i = j;
    }
}

static void infer_chords(const CompleteInputs& in, const CompleteParams& p,
                         const BeatFeatures& f, double gap_thresh,
                         CompleteProposal* out)
{
    if (in.beatmap->count < 2) return;
    std::vector<CompleteCand> kept;
    // Pending section candidates that already carry their chords own that
    // span: nothing else proposes chords there.
    size_t n_reserved = 0;
    for (const CompleteCand& c : out->cands)
        if (c.kind == CAND_SECTION && c.chord_n > 0) { kept.push_back(c); n_reserved++; }
    chords_from_sections(in, p, f, out, &kept);
    if (p.chord_runs)     chords_from_runs(in, p, f, gap_thresh, out, &kept);
    if (p.chord_fallback) chords_from_models(in, p, f, out, &kept);
    out->cands.insert(out->cands.end(), kept.begin() + n_reserved, kept.end());
}

// ===========================================================================
// Driver
// ===========================================================================

void complete_run(const CompleteInputs& in, const CompleteParams& p,
                  BeatChromaCache* cache, CompleteProposal* out)
{
    out->beat_times.clear();
    out->beat_conf.clear();
    out->chords.clear();
    out->onsets.clear();
    out->cands.clear();
    out->gap_count = 0;
    out->template_count = 0;
    out->status[0] = 0;

    const BeatMap* bm = in.beatmap;
    double med = median_ibi(bm);
    double gap_thresh = p.gap_factor * med;

    // Beat-synchronous chroma for everything already mapped
    {
        std::vector<double> times;
        times.reserve(bm->count);
        for (int i = 0; i < bm->count; i++) times.push_back(bm->beats[i].time);
        beat_chroma_ensure(cache, in.audio, times.data(), (int)times.size(), p.chroma);
    }

    // Onset timbre shapes for the whole track (vocabulary from the mapped
    // parts), whichever strategy is in use: the timbre strip shows them.
    ShapeAnalysis* shapes = shape_track();
    shape_analysis_ensure(shapes, in.audio, bm, in.duration, p.shape, p.beat_algo_idx);
    {
        std::vector<ShapeMark> marks;
        marks.reserve(shapes->onset_t.size());
        for (size_t i = 0; i < shapes->onset_t.size(); i++) {
            ShapeMark m;
            m.t = shapes->onset_t[i]; m.win = shapes->win;
            m.shape = shapes->onset_shape[i]; m.energy = shapes->onset_energy[i];
            m.conf = (m.shape >= 0 && shapes->vocab.valid)
                   ? shapes->onset_soft[i * shapes->vocab.k + m.shape] : 0.0f;
            marks.push_back(m);
        }
        shape_marks_set(SHAPE_SRC_ONSET, marks);
        std::vector<double> bt;
        for (int i = 0; i < bm->count; i++) bt.push_back(bm->beats[i].time);
        shape_marks_classify(SHAPE_SRC_BEAT, in.audio, bt.data(), (int)bt.size(), shapes->win);
    }

    int n_beats = 0, n_sec = 0, n_ch = 0;
    if (p.do_beats) {
        std::vector<Template> tm;
        build_templates(in, p, cache, shapes, gap_thresh, &tm);
        out->template_count = (int)tm.size();
        std::vector<Gap> gaps;
        find_gaps(in, med, gap_thresh, &gaps);
        out->gap_count = (int)gaps.size();
        for (const Gap& g : gaps) fill_gap(in, p, tm, shapes, g, out);
        n_beats = (int)out->beat_times.size();
        shape_marks_classify(SHAPE_SRC_PROPOSED, in.audio, out->beat_times.data(),
                             (int)out->beat_times.size(), shapes->win);
    } else {
        shape_marks_clear(SHAPE_SRC_PROPOSED);
    }
    BeatFeatures feats;
    if (p.do_sections || p.do_chords)
        build_beat_features(bm, cache, shapes, p.shape.slots_per_beat, &feats);
    if (p.do_sections) {
        size_t before = out->cands.size();
        infer_sections(in, p, feats, gap_thresh, out);
        n_sec = (int)(out->cands.size() - before);
    }
    if (p.do_chords) {
        size_t before = out->cands.size();
        infer_chords(in, p, feats, gap_thresh, out);
        n_ch = (int)(out->cands.size() - before);
    }

    // Region filter for section/chord candidates (beat gaps were clipped already)
    if (in.has_region) {
        std::vector<CompleteCand> kept;
        for (const CompleteCand& c : out->cands)
            if (c.kind == CAND_BEATS || complete_cand_in_range(c, in.region_start, in.region_end))
                kept.push_back(c);
        n_sec = n_ch = 0;
        for (const CompleteCand& c : kept) { if (c.kind == CAND_SECTION) n_sec++; if (c.kind == CAND_CHORDS) n_ch++; }
        out->cands.swap(kept);
    }

    // Rank within each kind by score (beats stay chronological: their order is
    // their meaning), kinds in stage order.
    std::stable_sort(out->cands.begin(), out->cands.end(),
                     [](const CompleteCand& x, const CompleteCand& y) {
                         if (x.kind != y.kind) return x.kind < y.kind;
                         if (x.kind == CAND_BEATS) return x.t0 < y.t0;
                         return x.score > y.score;
                     });

    snprintf(out->status, sizeof(out->status),
             "%d gap%s, %d template%s: %d beats, %d section%s, %d chord set%s proposed",
             out->gap_count, out->gap_count == 1 ? "" : "s",
             out->template_count, out->template_count == 1 ? "" : "s",
             n_beats, n_sec, n_sec == 1 ? "" : "s", n_ch, n_ch == 1 ? "" : "s");
}
