#include "chroma_algo.h"

const ChromaAlgoDesc CHROMA_ALGOS[] = {
    {
        "Goertzel / Hann",
        "Goertzel filters at exact note frequencies, Hann window (4096 samples)",
        chroma_goertzel_hann
    },
    {
        "Goertzel / Blackman-Harris",
        "Goertzel filters with 4-term Blackman-Harris window (-92 dB sidelobes)",
        chroma_goertzel_blackman
    },
    {
        "HPS",
        "Harmonic Product Spectrum: multiplies magnitude at f, 2f, 3f, 4f to suppress overtones",
        chroma_hps
    },
    {
        "NNLS Chroma",
        "Non-Negative Least Squares fit of harmonic templates (most accurate for polyphony)",
        chroma_nnls
    },
    {
        "Spectral Peaks",
        "Finds spectral peaks then groups harmonically-related ones to a single fundamental",
        chroma_peaks
    },
    {
        "Resonate",
        "Bank of 60 complex resonators updated per-sample (Francois); no FFT, no window function",
        chroma_resonate
    },
    {
        "HPS + Peaks",
        "Average of Harmonic Product Spectrum and Spectral Peaks: two harmonic-aware views,\n"
        "the best pair for telling chords apart in a mix",
        chroma_hps_peaks
    },
    {
        "Whitened Log-Freq",
        "Log-frequency spectrum with tuning correction, log compression and octave-wide\n"
        "spectral whitening (Cho & Bello / Mauch): the chord-recognition front end",
        chroma_whitened
    },
    {
        "Whitened + HPS/Peaks",
        "Average of the whitened log-frequency chroma and HPS+Peaks: the whitened view\n"
        "removes drums and timbre, the harmonic views sharpen the fundamentals",
        chroma_whitened_hps_peaks
    },
};

const int CHROMA_ALGO_COUNT = 9;

void chroma_whitened_hps_peaks(const float* pcm, uint64_t n, uint32_t ch, uint32_t sr,
                               double t0, double t1, float out[12])
{
    float a[12], b[12];
    chroma_whitened(pcm, n, ch, sr, t0, t1, a);
    chroma_hps_peaks(pcm, n, ch, sr, t0, t1, b);
    float mx = 0.0f;
    for (int i = 0; i < 12; i++) { out[i] = 0.5f * (a[i] + b[i]); if (out[i] > mx) mx = out[i]; }
    if (mx > 1e-9f) for (int i = 0; i < 12; i++) out[i] /= mx;
}

void chroma_hps_peaks(const float* pcm, uint64_t n, uint32_t ch, uint32_t sr,
                      double t0, double t1, float out[12])
{
    float a[12], b[12];
    chroma_hps(pcm, n, ch, sr, t0, t1, a);
    chroma_peaks(pcm, n, ch, sr, t0, t1, b);
    float mx = 0.0f;
    for (int i = 0; i < 12; i++) { out[i] = 0.5f * (a[i] + b[i]); if (out[i] > mx) mx = out[i]; }
    if (mx > 1e-9f) for (int i = 0; i < 12; i++) out[i] /= mx;
}
