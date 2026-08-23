#pragma once
#include <stdint.h>
#include <vector>

// ---------------------------------------------------------------------------
// Beat-synchronous chroma: one 12-vector per beat interval, cached.
//
// A beat's chroma is taken over the *body* of the interval, not its start:
// the first tens of milliseconds after a beat are usually a drum hit whose
// spread-spectrum noise swamps the pitch content.  The window skipped is the
// larger of `attack_ms` and `attack_frac` of the interval, but never more
// than half of it.
//
// The cache is keyed on (start, end) per beat plus the parameters in force;
// moving a beat only invalidates the one or two intervals that changed.
// ---------------------------------------------------------------------------

struct BeatChromaParams {
    int   algo_idx;      // index into CHROMA_ALGOS
    float attack_ms;     // skip at least this much of the start of each beat
    float attack_frac;   // ... and at least this fraction of the interval
};
void beat_chroma_params_defaults(BeatChromaParams* p);

struct BeatChromaEntry {
    double t0, t1;       // interval the vector was computed for
    float  v[12];        // normalised chroma, C..B
};

struct BeatChromaCache {
    std::vector<BeatChromaEntry> entries;   // entries[i] = interval beats[i]..beats[i+1]
    BeatChromaParams params;                // params the entries were computed with
    bool params_valid = false;
    int  recomputed_last = 0;               // stats: intervals recomputed by the last ensure()
};

struct AudioPcm {
    const float* pcm;
    uint64_t     frame_count;
    uint32_t     channels;
    uint32_t     sample_rate;
};

// Make entries[0..n-2] current for the beat times given (chronological).
// Only intervals whose bounds changed, or everything when params changed,
// are recomputed.  n < 2 leaves the cache empty.
void beat_chroma_ensure(BeatChromaCache* c, const AudioPcm& a,
                        const double* beat_times, int n,
                        const BeatChromaParams& p);

// Chroma of one arbitrary interval with the same attack discount (uncached);
// used for audio that has no beats yet.
void beat_chroma_interval(const AudioPcm& a, double t0, double t1,
                          const BeatChromaParams& p, float out[12]);

// Cosine similarity of two chroma vectors (0 when either is silent).
float chroma_cosine(const float a[12], const float b[12]);

void beat_chroma_clear(BeatChromaCache* c);
