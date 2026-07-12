#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Common interface for all chroma analysis algorithms.
//
// Every algorithm receives the same inputs and fills result[12] with
// normalised chroma intensities [0,1] for pitch classes C=0 .. B=11.
// The function must handle null pcm gracefully (zero-fill result).
// ---------------------------------------------------------------------------
typedef void (*ChromaFn)(
    const float* pcm,           // stereo-interleaved f32 samples
    uint64_t     frame_count,   // total frames in buffer
    uint32_t     channels,      // channel count (1 or 2)
    uint32_t     sample_rate,   // Hz
    double       t_start,       // analysis window start (seconds)
    double       t_end,         // analysis window end   (seconds)
    float        result[12]     // out: normalised chroma [0,1], C..B
);

struct ChromaAlgoDesc {
    const char* name;   // shown in the selector combo
    const char* tip;    // tooltip / one-line description
    ChromaFn    fn;
};

// Forward declarations (each algorithm is defined in its own .cpp)
void chroma_goertzel_hann    (const float*, uint64_t, uint32_t, uint32_t, double, double, float[12]);
void chroma_goertzel_blackman(const float*, uint64_t, uint32_t, uint32_t, double, double, float[12]);
void chroma_hps              (const float*, uint64_t, uint32_t, uint32_t, double, double, float[12]);
void chroma_nnls             (const float*, uint64_t, uint32_t, uint32_t, double, double, float[12]);
void chroma_peaks            (const float*, uint64_t, uint32_t, uint32_t, double, double, float[12]);

// Registration table – defined in chroma_algo.cpp
extern const ChromaAlgoDesc CHROMA_ALGOS[];
extern const int            CHROMA_ALGO_COUNT;
