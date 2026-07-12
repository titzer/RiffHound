#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Output: auto-detected beat candidates
// ---------------------------------------------------------------------------
static const int MAX_BEAT_CANDS = 2048;

struct AutoBeatList {
    // Tempo-regularised beat positions (primary output of each algorithm)
    double beat_times   [MAX_BEAT_CANDS];
    bool   beat_selected[MAX_BEAT_CANDS];
    int    beat_count;

    // Raw onset times (shown as subtle ticks when show_raw is enabled)
    double onset_times[MAX_BEAT_CANDS];
    int    onset_count;

    // Estimated tempo in BPM; 0 = could not determine
    float  estimated_bpm;
};

void autobeat_init(AutoBeatList* ab);

// ---------------------------------------------------------------------------
// Algorithm interface
// ---------------------------------------------------------------------------
struct BeatAlgoParams {
    float min_bpm;          // minimum expected tempo          (default  60)
    float max_bpm;          // maximum expected tempo          (default 200)
    float onset_threshold;  // flux peak threshold multiplier  (default 1.5)
    float dp_tightness;     // Ellis DP temporal adherence     (default 400)
    float pre_onset_ms;     // shift beats this far before onset peak (default 30)

    // Optional: accepted beats within the window to bootstrap tempo/phase.
    // When seed_count >= 2 the algorithm derives tau from these rather than
    // autocorrelation, and phase-aligns the DP grid to match.
    const double* seed_times;   // array of beat times; nullptr = no seeds
    int           seed_count;
};

typedef void (*BeatAlgoFn)(
    const float* pcm,
    uint64_t     frame_count,
    uint32_t     channels,
    uint32_t     sample_rate,
    double       t_start,
    double       t_end,
    const BeatAlgoParams* params,
    AutoBeatList* out
);

struct BeatAlgoDesc {
    const char* name;
    const char* tip;
    BeatAlgoFn  fn;
};

// ---------------------------------------------------------------------------
// Algorithm declarations
// ---------------------------------------------------------------------------
void beat_spectral_flux(const float* pcm, uint64_t frame_count,
                        uint32_t channels, uint32_t sample_rate,
                        double t_start, double t_end,
                        const BeatAlgoParams* params,
                        AutoBeatList* out);

extern const BeatAlgoDesc BEAT_ALGOS[];
extern const int           BEAT_ALGO_COUNT;
