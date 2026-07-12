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
};

const int CHROMA_ALGO_COUNT = 5;
