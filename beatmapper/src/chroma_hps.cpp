#include "chroma_algo.h"
#include "chroma_fft.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Harmonic Product Spectrum (HPS) chroma.
//
// For each FFT frame, HPS[k] = |X[k]| * |X[2k]| * |X[3k]| * |X[4k]| * |X[5k]|.
// Bins where the fundamental and its first four harmonics all have energy are
// strongly amplified; isolated harmonics of other notes are suppressed because
// their half-, third-, quarter-frequency bins are typically quiet.
//
// The HPS values in the C2–C6 range (65–1047 Hz) are accumulated into the
// 12 pitch classes using the same midi-rounding approach as the FFT chroma.
// ---------------------------------------------------------------------------

static const int HPS_N     = 8192;  // FFT window size
static const int HPS_ORDER = 5;     // number of harmonics to multiply

void chroma_hps(const float* pcm, uint64_t frame_count, uint32_t ch,
                uint32_t sr, double t0, double t1, float result[12])
{
    memset(result, 0, 12 * sizeof(float));
    if (!pcm || frame_count == 0 || ch == 0 || sr == 0) return;
    if (t1 - t0 > 8.0) t0 = t1 - 8.0;

    int64_t fs = (int64_t)(t0 * sr); if (fs < 0) fs = 0;
    int64_t fe = (int64_t)(t1 * sr); if (fe > (int64_t)frame_count) fe = (int64_t)frame_count;
    if (fe - fs < HPS_N) return;

    // Static scratch buffers
    static float s_re[HPS_N], s_im[HPS_N];
    static float s_mag[HPS_N / 2];
    static float s_win[HPS_N];
    static bool  s_win_init = false;
    if (!s_win_init) {
        for (int i = 0; i < HPS_N; i++)
            s_win[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979f * i / (HPS_N - 1)));
        s_win_init = true;
    }

    const int   BINS         = HPS_N / 2;
    const float freq_per_bin = (float)sr / (float)HPS_N;
    // Maximum bin for which a full HPS_ORDER-way product is valid
    const int   MAX_FUND_BIN = BINS / HPS_ORDER;

    double power[12] = {};

    for (int64_t pos = fs; pos + HPS_N <= fe; pos += HPS_N) {
        // Stereo → mono, apply window
        for (int i = 0; i < HPS_N; i++) {
            float s = 0.0f;
            for (uint32_t c = 0; c < ch; c++) s += pcm[(pos + i) * ch + c];
            s_re[i] = (s / (float)ch) * s_win[i];
            s_im[i] = 0.0f;
        }
        chroma_fft(s_re, s_im, HPS_N);

        // Magnitude spectrum
        for (int k = 0; k < BINS; k++)
            s_mag[k] = sqrtf(s_re[k]*s_re[k] + s_im[k]*s_im[k]);

        // Accumulate HPS into pitch classes (C2..C6: 65..1047 Hz)
        for (int k = 1; k < MAX_FUND_BIN; k++) {
            float freq = (float)k * freq_per_bin;
            if (freq < 60.0f || freq > 1100.0f) continue;

            // HPS product: multiply magnitudes at fundamental and harmonics
            float hps = s_mag[k];
            for (int h = 2; h <= HPS_ORDER; h++)
                hps *= s_mag[k * h];

            // Map to pitch class
            float midi = 12.0f * log2f(freq / 440.0f) + 69.0f;
            int   pc   = ((int)roundf(midi) % 12 + 12) % 12;
            power[pc] += (double)hps;
        }
    }

    // Normalise
    double mx = 1e-30;
    for (int i = 0; i < 12; i++) if (power[i] > mx) mx = power[i];
    for (int i = 0; i < 12; i++) {
        float db = 10.0f * log10f((float)(power[i] / mx) + 1e-30f);
        float v  = (db + 30.0f) / 30.0f;
        result[i] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
}
