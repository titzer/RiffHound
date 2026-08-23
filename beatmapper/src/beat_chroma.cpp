#include "beat_chroma.h"
#include "chroma_algo.h"
#include <math.h>
#include <string.h>

void beat_chroma_params_defaults(BeatChromaParams* p) {
    p->algo_idx    = 3;       // NNLS Chroma
    p->attack_ms   = 60.0f;
    p->attack_frac = 0.15f;
}

static bool params_equal(const BeatChromaParams& a, const BeatChromaParams& b) {
    return a.algo_idx == b.algo_idx && a.attack_ms == b.attack_ms &&
           a.attack_frac == b.attack_frac;
}

void beat_chroma_interval(const AudioPcm& a, double t0, double t1,
                          const BeatChromaParams& p, float out[12])
{
    memset(out, 0, sizeof(float) * 12);
    if (!a.pcm || t1 <= t0) return;
    int algo = p.algo_idx;
    if (algo < 0 || algo >= CHROMA_ALGO_COUNT) algo = 0;

    double len  = t1 - t0;
    double skip = p.attack_ms * 0.001;
    if (p.attack_frac * len > skip) skip = p.attack_frac * len;
    if (skip > len * 0.5) skip = len * 0.5;
    CHROMA_ALGOS[algo].fn(a.pcm, a.frame_count, a.channels, a.sample_rate,
                          t0 + skip, t1, out);
}

void beat_chroma_ensure(BeatChromaCache* c, const AudioPcm& a,
                        const double* beat_times, int n,
                        const BeatChromaParams& p)
{
    c->recomputed_last = 0;
    if (n < 2 || !a.pcm) { c->entries.clear(); c->params = p; c->params_valid = true; return; }

    bool all = !c->params_valid || !params_equal(c->params, p);
    int  n_int = n - 1;

    // Preserve matching entries by interval bounds: a moved beat changes the
    // bounds of its two neighbouring intervals only, but an inserted beat
    // shifts indices, so match by (t0, t1) rather than by position.
    std::vector<BeatChromaEntry> fresh(n_int);
    size_t j = 0;   // scan pointer into the old (chronological) entries
    for (int i = 0; i < n_int; i++) {
        double t0 = beat_times[i], t1 = beat_times[i + 1];
        bool hit = false;
        if (!all) {
            while (j < c->entries.size() && c->entries[j].t0 < t0 - 1e-9) j++;
            if (j < c->entries.size() &&
                fabs(c->entries[j].t0 - t0) < 1e-9 && fabs(c->entries[j].t1 - t1) < 1e-9) {
                fresh[i] = c->entries[j];
                hit = true;
            }
        }
        if (!hit) {
            fresh[i].t0 = t0;
            fresh[i].t1 = t1;
            beat_chroma_interval(a, t0, t1, p, fresh[i].v);
            c->recomputed_last++;
        }
    }
    c->entries.swap(fresh);
    c->params       = p;
    c->params_valid = true;
}

float chroma_cosine(const float a[12], const float b[12]) {
    float dot = 0, na = 0, nb = 0;
    for (int i = 0; i < 12; i++) { dot += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; }
    if (na < 1e-12f || nb < 1e-12f) return 0.0f;
    return dot / sqrtf(na * nb);
}

void beat_chroma_clear(BeatChromaCache* c) {
    c->entries.clear();
    c->params_valid = false;
    c->recomputed_last = 0;
}
