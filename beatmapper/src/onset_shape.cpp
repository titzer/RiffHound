#include "onset_shape.h"
#include "beatmap.h"
#include "beat_algo.h"
#include "chroma_fft.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <mutex>

void shape_params_defaults(ShapeParams* p) {
    p->k               = 5;
    p->window_frac     = 0.25f;
    p->slots_per_beat  = 12;
    p->bands           = 24;
    p->frames          = 3;
    p->low_weight      = 1.5f;
    p->high_weight     = 1.5f;
    p->onset_threshold = 1.0f;
}

static bool params_equal(const ShapeParams& a, const ShapeParams& b) {
    return memcmp(&a, &b, sizeof(ShapeParams)) == 0;
}

// ===========================================================================
// Descriptor
// ===========================================================================

static const double F_LO = 40.0, F_HI = 12000.0;

// Log-band energies of one frame starting at sample fs (flen samples, Hann,
// zero-padded to nfft).  Returns the frame's mean square for loudness.
static double band_frame(const AudioPcm& a, int64_t fs, int64_t flen, int nfft,
                         const std::vector<int>& edge, int bands,
                         std::vector<float>& re, std::vector<float>& im, float* out)
{
    double ms = 0.0; int64_t n = 0;
    for (int i = 0; i < nfft; i++) {
        float v = 0.0f;
        int64_t idx = fs + i;
        if (i < flen && idx >= 0 && (uint64_t)idx < a.frame_count) {
            for (uint32_t c = 0; c < a.channels; c++) v += a.pcm[(uint64_t)idx * a.channels + c];
            v /= (float)a.channels;
            ms += (double)v * v; n++;
        }
        float w = (i < flen) ? 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (float)(flen > 1 ? flen - 1 : 1))) : 0.0f;
        re[i] = v * w; im[i] = 0.0f;
    }
    chroma_fft(re.data(), im.data(), nfft);
    for (int b = 0; b < bands; b++) {
        double e = 0.0;
        for (int k = edge[b]; k < edge[b + 1]; k++) e += (double)re[k] * re[k] + (double)im[k] * im[k];
        e /= (edge[b + 1] - edge[b]);
        out[b] = log10f(1.0f + (float)e * 1e4f);
    }
    return n ? ms / n : 0.0;
}

void shape_descriptor(const AudioPcm& a, double t, double win, const ShapeParams& p,
                      std::vector<float>* out, float* energy)
{
    int bands  = p.bands  < 4 ? 4 : (p.bands > 64 ? 64 : p.bands);
    int frames = p.frames < 1 ? 1 : (p.frames > 8 ? 8 : p.frames);
    out->assign((size_t)bands * frames, 0.0f);
    if (energy) *energy = -120.0f;
    if (!a.pcm || win <= 0.0 || a.sample_rate == 0) return;

    int64_t s0   = (int64_t)(t * a.sample_rate);
    int64_t wlen = (int64_t)(win * a.sample_rate);
    if (wlen < 64) wlen = 64;
    int64_t flen = wlen / frames;
    int nfft = 256;
    while (nfft < flen) nfft <<= 1;
    if (nfft > 8192) nfft = 8192;

    std::vector<float> re(nfft), im(nfft);
    std::vector<int> edge(bands + 1);
    for (int b = 0; b <= bands; b++) {
        double f = F_LO * pow(F_HI / F_LO, (double)b / bands);
        int bin = (int)(f * nfft / a.sample_rate);
        if (bin < 1) bin = 1;
        if (bin > nfft / 2) bin = nfft / 2;
        edge[b] = bin;
    }
    for (int b = 1; b <= bands; b++) if (edge[b] <= edge[b - 1]) edge[b] = edge[b - 1] + 1;

    // Reference: the same-length stretch just before the onset, so the
    // descriptor is what the hit *adds* rather than what was already ringing.
    std::vector<float> ref(bands, 0.0f), tmp(bands);
    for (int f = 0; f < frames; f++) {
        band_frame(a, s0 - wlen + f * flen, flen, nfft, edge, bands, re, im, tmp.data());
        for (int b = 0; b < bands; b++) ref[b] += tmp[b] / frames;
    }

    double ms = 0.0;
    for (int f = 0; f < frames; f++) {
        ms += band_frame(a, s0 + f * flen, flen, nfft, edge, bands, re, im, &(*out)[(size_t)f * bands]);
        for (int b = 0; b < bands; b++) {
            float d = (*out)[(size_t)f * bands + b] - ref[b];
            (*out)[(size_t)f * bands + b] = d > 0.0f ? d : 0.0f;   // only what appeared
        }
    }
    if (energy) *energy = 10.0f * log10f((float)(ms / frames) + 1e-12f);

    // Emphasise the ends of the spectrum, then unit length (level-invariant)
    for (int f = 0; f < frames; f++)
        for (int b = 0; b < bands; b++) {
            double fc = F_LO * pow(F_HI / F_LO, (b + 0.5) / bands);
            float w = 1.0f;
            if (fc < 200.0)  w = p.low_weight;
            if (fc > 4000.0) w = p.high_weight;
            (*out)[(size_t)f * bands + b] *= w;
        }
    float n2 = 0.0f;
    for (float v : *out) n2 += v * v;
    if (n2 > 1e-12f) { float inv = 1.0f / sqrtf(n2); for (float& v : *out) v *= inv; }
}

// ===========================================================================
// Vocabulary
// ===========================================================================

static float dist2(const float* a, const float* b, int dim) {
    float d = 0.0f;
    for (int i = 0; i < dim; i++) { float x = a[i] - b[i]; d += x * x; }
    return d;
}

// Deterministic LCG so repeated runs give the same vocabulary.
static uint32_t s_rng = 12345u;
static uint32_t rng_next() { s_rng = s_rng * 1664525u + 1013904223u; return s_rng; }
static float    rng_unit() { return (rng_next() >> 8) / 16777216.0f; }

bool shape_train(const float* feats_raw, const float* energies, int n, int dim, int k,
                 ShapeVocab* out)
{
    if (k < 1) k = 1;
    if (k > SHAPE_MAX_K) k = SHAPE_MAX_K;
    out->valid = false;
    if (n < k || dim < 1) return false;

    // Standardise each dimension across the training set so the variation
    // *between* hits drives the clustering, not the spectral tilt they share.
    std::vector<float> mean(dim, 0.0f), inv_std(dim, 1.0f);
    for (int i = 0; i < n; i++) for (int d = 0; d < dim; d++) mean[d] += feats_raw[(size_t)i * dim + d];
    for (int d = 0; d < dim; d++) mean[d] /= n;
    for (int i = 0; i < n; i++) for (int d = 0; d < dim; d++) { float x = feats_raw[(size_t)i * dim + d] - mean[d]; inv_std[d] += x * x; }
    for (int d = 0; d < dim; d++) { float v = (inv_std[d] - 1.0f) / n; inv_std[d] = v > 1e-8f ? 1.0f / sqrtf(v) : 0.0f; }
    std::vector<float> zf((size_t)n * dim);
    for (int i = 0; i < n; i++) for (int d = 0; d < dim; d++)
        zf[(size_t)i * dim + d] = (feats_raw[(size_t)i * dim + d] - mean[d]) * inv_std[d];
    const float* feats = zf.data();

    std::vector<float> best_cent;
    std::vector<int>   best_assign;
    float best_sse = 1e30f;
    s_rng = 12345u;

    for (int restart = 0; restart < 3; restart++) {
        std::vector<float> cent((size_t)k * dim);
        std::vector<float> dmin(n, 1e30f);
        // k-means++ seeding
        int first = (int)(rng_unit() * n) % n;
        memcpy(&cent[0], feats + (size_t)first * dim, sizeof(float) * dim);
        for (int c = 1; c < k; c++) {
            double total = 0.0;
            for (int i = 0; i < n; i++) {
                float d = dist2(feats + (size_t)i * dim, &cent[(size_t)(c - 1) * dim], dim);
                if (d < dmin[i]) dmin[i] = d;
                total += dmin[i];
            }
            double r = rng_unit() * total, acc = 0.0;
            int pick = n - 1;
            for (int i = 0; i < n; i++) { acc += dmin[i]; if (acc >= r) { pick = i; break; } }
            memcpy(&cent[(size_t)c * dim], feats + (size_t)pick * dim, sizeof(float) * dim);
        }
        std::vector<int> assign(n, 0);
        for (int iter = 0; iter < 40; iter++) {
            bool changed = false;
            for (int i = 0; i < n; i++) {
                int bc = 0; float bd = 1e30f;
                for (int c = 0; c < k; c++) {
                    float d = dist2(feats + (size_t)i * dim, &cent[(size_t)c * dim], dim);
                    if (d < bd) { bd = d; bc = c; }
                }
                if (assign[i] != bc) { assign[i] = bc; changed = true; }
            }
            std::vector<float> sum((size_t)k * dim, 0.0f);
            std::vector<int>   cnt(k, 0);
            for (int i = 0; i < n; i++) {
                cnt[assign[i]]++;
                for (int d = 0; d < dim; d++) sum[(size_t)assign[i] * dim + d] += feats[(size_t)i * dim + d];
            }
            for (int c = 0; c < k; c++) {
                if (cnt[c] == 0) {   // empty cluster: re-seed on the farthest point
                    int far = 0; float fd = -1.0f;
                    for (int i = 0; i < n; i++) {
                        float d = dist2(feats + (size_t)i * dim, &cent[(size_t)assign[i] * dim], dim);
                        if (d > fd) { fd = d; far = i; }
                    }
                    memcpy(&cent[(size_t)c * dim], feats + (size_t)far * dim, sizeof(float) * dim);
                    changed = true;
                    continue;
                }
                for (int d = 0; d < dim; d++) cent[(size_t)c * dim + d] = sum[(size_t)c * dim + d] / cnt[c];
            }
            if (!changed) break;
        }
        float sse = 0.0f;
        for (int i = 0; i < n; i++) sse += dist2(feats + (size_t)i * dim, &cent[(size_t)assign[i] * dim], dim);
        if (sse < best_sse) { best_sse = sse; best_cent = cent; best_assign = assign; }
    }

    // Order shapes loudest first
    std::vector<float> en(k, 0.0f);
    std::vector<int>   cnt(k, 0);
    for (int i = 0; i < n; i++) { en[best_assign[i]] += energies ? energies[i] : 0.0f; cnt[best_assign[i]]++; }
    for (int c = 0; c < k; c++) en[c] = cnt[c] ? en[c] / cnt[c] : -120.0f;
    std::vector<int> order(k);
    for (int c = 0; c < k; c++) order[c] = c;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return en[a] > en[b]; });

    out->k = k; out->dim = dim;
    out->mean = mean; out->inv_std = inv_std;
    // Membership sharpness: one "typical" nearest-centroid distance
    {
        double acc = 0.0;
        for (int i = 0; i < n; i++) acc += dist2(feats + (size_t)i * dim, &best_cent[(size_t)best_assign[i] * dim], dim);
        out->scale = (float)(acc / n) > 1e-6f ? (float)(n / acc) : 1.0f;
    }
    out->cent.resize((size_t)k * dim);
    out->count.resize(k);
    out->energy.resize(k);
    for (int c = 0; c < k; c++) {
        memcpy(&out->cent[(size_t)c * dim], &best_cent[(size_t)order[c] * dim], sizeof(float) * dim);
        out->count[c]  = cnt[order[c]];
        out->energy[c] = en[order[c]];
    }
    out->valid = true;
    return true;
}

int shape_classify(const ShapeVocab& v, const float* feat, float* soft) {
    if (!v.valid) { if (soft) for (int c = 0; c < SHAPE_MAX_K; c++) soft[c] = 0.0f; return -1; }
    float z[256];
    int dim = v.dim > 256 ? 256 : v.dim;
    for (int i = 0; i < dim; i++) z[i] = (feat[i] - v.mean[i]) * v.inv_std[i];
    float d[SHAPE_MAX_K];
    int best = 0; float bd = 1e30f;
    for (int c = 0; c < v.k; c++) {
        d[c] = dist2(z, &v.cent[(size_t)c * v.dim], dim);
        if (d[c] < bd) { bd = d[c]; best = c; }
    }
    if (soft) {
        // Softmax over distance in units of the typical within-cluster distance
        float sum = 0.0f;
        for (int c = 0; c < v.k; c++) { soft[c] = expf(-(d[c] - bd) * v.scale); sum += soft[c]; }
        for (int c = 0; c < v.k; c++) soft[c] /= sum;
        for (int c = v.k; c < SHAPE_MAX_K; c++) soft[c] = 0.0f;
    }
    return best;
}

// ===========================================================================
// Track-wide analysis
// ===========================================================================

static ShapeAnalysis s_track;
static ShapeParams   s_params;
static bool          s_params_init = false;

ShapeAnalysis* shape_track() { return &s_track; }
ShapeParams*   shape_params() {
    if (!s_params_init) { shape_params_defaults(&s_params); s_params_init = true; }
    return &s_params;
}

void shape_analysis_clear(ShapeAnalysis* sa) {
    sa->vocab = ShapeVocab();
    sa->onset_t.clear(); sa->onset_energy.clear(); sa->onset_shape.clear();
    sa->onset_soft.clear(); sa->onset_feat.clear();
    sa->key_valid = false;
}

int shape_onset_at(const ShapeAnalysis& sa, double t) {
    return (int)(std::lower_bound(sa.onset_t.begin(), sa.onset_t.end(), t) - sa.onset_t.begin());
}

// Onsets over the whole track, in chunks (the detector's output is capped).
static void harvest_onsets(const AudioPcm& a, double duration, float threshold,
                           int algo_idx, std::vector<double>* out)
{
    out->clear();
    if (!a.pcm || algo_idx < 0 || algo_idx >= BEAT_ALGO_COUNT) return;
    static AutoBeatList ab;
    const double CHUNK = 20.0, PAD = 0.5;
    BeatAlgoParams bp = {};
    bp.min_bpm = 60; bp.max_bpm = 200; bp.onset_threshold = threshold;
    bp.dp_tightness = 50; bp.pre_onset_ms = 0;
    for (double t0 = 0.0; t0 < duration; t0 += CHUNK) {
        double t1 = std::min(duration, t0 + CHUNK + PAD);
        if (t1 - t0 < 0.3) break;
        autobeat_init(&ab);
        BEAT_ALGOS[algo_idx].fn(a.pcm, a.frame_count, a.channels, a.sample_rate, t0, t1, &bp, &ab);
        for (int i = 0; i < ab.onset_count; i++) {
            double t = ab.onset_times[i];
            if (t >= t0 + CHUNK) continue;                  // belongs to the next chunk
            if (!out->empty() && t - out->back() < 0.02) continue;
            out->push_back(t);
        }
    }
    std::sort(out->begin(), out->end());
}

bool shape_analysis_ensure(ShapeAnalysis* sa, const AudioPcm& a, const BeatMap* bm,
                           double duration, const ShapeParams& p, int beat_algo_idx)
{
    if (!a.pcm) { shape_analysis_clear(sa); return false; }
    double checksum = 0.0;
    for (int i = 0; i < bm->count; i++) checksum += bm->beats[i].time * (i + 1);
    if (sa->key_valid && sa->key_beats == bm->count && sa->key_checksum == checksum &&
        params_equal(sa->key_params, p) && sa->key_algo == beat_algo_idx &&
        sa->key_frames == a.frame_count)
        return sa->vocab.valid;

    shape_analysis_clear(sa);

    // Window: a fraction of the median beat (half a second if there is no map)
    double med = 0.5;
    if (bm->count >= 2) {
        std::vector<double> d;
        for (int i = 1; i < bm->count; i++) d.push_back(bm->beats[i].time - bm->beats[i - 1].time);
        std::sort(d.begin(), d.end());
        med = d[d.size() / 2];
    }
    sa->win = p.window_frac * med;
    if (sa->win < 0.02) sa->win = 0.02;

    harvest_onsets(a, duration, p.onset_threshold, beat_algo_idx, &sa->onset_t);
    int n = (int)sa->onset_t.size();
    int dim = 0;
    std::vector<float> f;
    sa->onset_energy.resize(n);
    for (int i = 0; i < n; i++) {
        shape_descriptor(a, sa->onset_t[i], sa->win, p, &f, &sa->onset_energy[i]);
        if (!dim) { dim = (int)f.size(); sa->onset_feat.resize((size_t)n * dim); }
        memcpy(&sa->onset_feat[(size_t)i * dim], f.data(), sizeof(float) * dim);
    }

    // Training set: onsets inside mapped stretches (where the map says what the
    // rhythm is), falling back to every onset when the map is too thin.
    std::vector<float> tf, te;
    if (bm->count >= 2) {
        double gap_thresh = 1.8 * med;
        for (int i = 0; i < n; i++) {
            double t = sa->onset_t[i];
            int j = (int)(std::lower_bound(bm->beats, bm->beats + bm->count, t,
                          [](const Beat& b, double v) { return b.time < v; }) - bm->beats);
            bool inside = false;
            if (j > 0 && j < bm->count && bm->beats[j].time - bm->beats[j - 1].time <= gap_thresh)
                inside = true;
            if (!inside) continue;
            tf.insert(tf.end(), &sa->onset_feat[(size_t)i * dim], &sa->onset_feat[(size_t)(i + 1) * dim]);
            te.push_back(sa->onset_energy[i]);
        }
    }
    if ((int)te.size() < p.k * 4) { tf = sa->onset_feat; te = sa->onset_energy; }
    shape_train(tf.data(), te.data(), (int)te.size(), dim, p.k, &sa->vocab);

    sa->onset_shape.resize(n);
    sa->onset_soft.assign((size_t)n * (sa->vocab.valid ? sa->vocab.k : 1), 0.0f);
    for (int i = 0; i < n; i++) {
        float soft[SHAPE_MAX_K];
        sa->onset_shape[i] = shape_classify(sa->vocab, &sa->onset_feat[(size_t)i * dim], soft);
        if (sa->vocab.valid)
            memcpy(&sa->onset_soft[(size_t)i * sa->vocab.k], soft, sizeof(float) * sa->vocab.k);
    }

    sa->key_valid = true;
    sa->key_beats = bm->count;
    sa->key_checksum = checksum;
    sa->key_params = p;
    sa->key_algo = beat_algo_idx;
    sa->key_frames = a.frame_count;
    return sa->vocab.valid;
}

// ===========================================================================
// Marks
// ===========================================================================

// Marks are written by whichever thread runs an analysis (the Complete Track
// background worker included) and read every frame by the UI.  Writers fill a
// back buffer under the lock; the UI-thread getter swaps a dirty back buffer
// in, so the reference it returns is only ever touched by the UI thread.
static std::vector<ShapeMark> s_marks[SHAPE_SRC_COUNT];        // front: UI only
static std::vector<ShapeMark> s_marks_back[SHAPE_SRC_COUNT];
static bool                   s_marks_dirty[SHAPE_SRC_COUNT] = {};
static std::mutex             s_marks_mu;

void shape_marks_set(ShapeSource src, const std::vector<ShapeMark>& marks) {
    if (src < 0 || src >= SHAPE_SRC_COUNT) return;
    std::lock_guard<std::mutex> lk(s_marks_mu);
    s_marks_back[src]  = marks;
    s_marks_dirty[src] = true;
}
void shape_marks_clear(ShapeSource src) {
    if (src < 0 || src >= SHAPE_SRC_COUNT) return;
    std::lock_guard<std::mutex> lk(s_marks_mu);
    s_marks_back[src].clear();
    s_marks_dirty[src] = true;
}
void shape_marks_clear_all() {
    for (int i = 0; i < SHAPE_SRC_COUNT; i++) shape_marks_clear((ShapeSource)i);
}
const std::vector<ShapeMark>& shape_marks(ShapeSource src) {
    static const std::vector<ShapeMark> empty;
    if (src < 0 || src >= SHAPE_SRC_COUNT) return empty;
    std::lock_guard<std::mutex> lk(s_marks_mu);
    if (s_marks_dirty[src]) {
        s_marks[src].swap(s_marks_back[src]);
        s_marks_back[src].clear();
        s_marks_dirty[src] = false;
    }
    return s_marks[src];
}

void shape_marks_classify(ShapeSource src, const AudioPcm& a, const double* times, int n,
                          double win)
{
    std::vector<ShapeMark> marks;
    marks.reserve(n);
    const ShapeParams& p = *shape_params();
    std::vector<float> f;
    for (int i = 0; i < n; i++) {
        ShapeMark m;
        m.t = times[i]; m.win = win;
        m.shape = -1; m.conf = 0.0f; m.energy = -120.0f;
        if (a.pcm) {
            shape_descriptor(a, m.t, win, p, &f, &m.energy);
            if (s_track.vocab.valid && (int)f.size() == s_track.vocab.dim) {
                float soft[SHAPE_MAX_K];
                m.shape = shape_classify(s_track.vocab, f.data(), soft);
                m.conf  = m.shape >= 0 ? soft[m.shape] : 0.0f;
            }
        }
        marks.push_back(m);
    }
    shape_marks_set(src, marks);
}
