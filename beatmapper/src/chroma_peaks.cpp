#include "chroma_algo.h"
#include "chroma_fft.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Spectral peak-picking + harmonic grouping chroma.
//
// Per frame:
//   1. Compute the magnitude spectrum.
//   2. Find local maxima above a noise threshold.
//   3. Sort peaks by frequency (ascending) so fundamentals are encountered
//      before their harmonics.
//   4. For each peak, check if it is a harmonic (integer multiple within
//      HARM_TOL) of any already-identified fundamental.  If yes, skip it
//      (or add a small bonus to the fundamental's power); if no, add it as
//      a new fundamental.
//   5. Map each surviving fundamental frequency to a pitch class (C..B)
//      and accumulate its magnitude into the chroma vector.
//
// This approach removes most harmonic bleed at the cost of missing cases
// where the fundamental is weaker than its overtones.
// ---------------------------------------------------------------------------

static const int   PEAKS_N      = 8192;   // FFT window size
static const int   MAX_PEAKS    = 300;    // max peaks per frame
static const float PEAK_THRESH  = 0.04f;  // min peak magnitude as fraction of frame max
static const float HARM_TOL     = 0.03f;  // harmonic ratio tolerance (3%)

struct Peak { float freq; float mag; };

// Compare by frequency ascending (for qsort)
static int peak_cmp_freq(const void* a, const void* b)
{
    float fa = ((const Peak*)a)->freq;
    float fb = ((const Peak*)b)->freq;
    return (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
}

void chroma_peaks(const float* pcm, uint64_t frame_count, uint32_t ch,
                   uint32_t sr, double t0, double t1, float result[12])
{
    memset(result, 0, 12 * sizeof(float));
    if (!pcm || frame_count == 0 || ch == 0 || sr == 0) return;
    if (t1 - t0 > 8.0) t0 = t1 - 8.0;

    int64_t fs = (int64_t)(t0 * sr); if (fs < 0) fs = 0;
    int64_t fe = (int64_t)(t1 * sr); if (fe > (int64_t)frame_count) fe = (int64_t)frame_count;
    if (fe - fs < PEAKS_N) return;

    static float s_re[PEAKS_N], s_im[PEAKS_N], s_mag[PEAKS_N / 2];
    static float s_win[PEAKS_N];
    static bool  s_win_init = false;
    if (!s_win_init) {
        for (int i = 0; i < PEAKS_N; i++)
            s_win[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979f * i / (PEAKS_N - 1)));
        s_win_init = true;
    }

    const int   BINS         = PEAKS_N / 2;
    const float freq_per_bin = (float)sr / (float)PEAKS_N;

    double power[12] = {};

    Peak s_peaks[MAX_PEAKS];
    // Fundamentals identified this frame
    float s_fund_freq[MAX_PEAKS];
    float s_fund_mag [MAX_PEAKS];

    for (int64_t pos = fs; pos + PEAKS_N <= fe; pos += PEAKS_N) {
        // Stereo → mono + window
        for (int i = 0; i < PEAKS_N; i++) {
            float s = 0.0f;
            for (uint32_t c = 0; c < ch; c++) s += pcm[(pos + i) * ch + c];
            s_re[i] = (s / (float)ch) * s_win[i];
            s_im[i] = 0.0f;
        }
        chroma_fft(s_re, s_im, PEAKS_N);

        // Magnitude spectrum + frame max
        float max_mag = 1e-30f;
        for (int k = 0; k < BINS; k++) {
            s_mag[k] = sqrtf(s_re[k]*s_re[k] + s_im[k]*s_im[k]);
            if (s_mag[k] > max_mag) max_mag = s_mag[k];
        }

        // Collect local maxima in C2–C6 range (65–1047 Hz)
        int   n_peaks = 0;
        float thresh  = max_mag * PEAK_THRESH;
        for (int k = 1; k < BINS - 1 && n_peaks < MAX_PEAKS; k++) {
            float f = (float)k * freq_per_bin;
            if (f < 60.0f || f > 1100.0f) continue;
            if (s_mag[k] > s_mag[k-1] && s_mag[k] > s_mag[k+1] && s_mag[k] >= thresh)
                s_peaks[n_peaks++] = { f, s_mag[k] };
        }

        // Sort by frequency ascending so fundamentals come before harmonics
        qsort(s_peaks, (size_t)n_peaks, sizeof(Peak), peak_cmp_freq);

        // Harmonic grouping
        int n_fund = 0;
        for (int i = 0; i < n_peaks; i++) {
            float f   = s_peaks[i].freq;
            bool  harm = false;

            for (int j = 0; j < n_fund; j++) {
                float F     = s_fund_freq[j];
                float ratio = f / F;
                int   h     = (int)roundf(ratio);
                if (h >= 2 && fabsf(ratio - (float)h) < HARM_TOL * (float)h) {
                    // This peak is approximately h*F — a harmonic of fundamental j.
                    // Give the fundamental a small bonus to reward confirmed harmonics.
                    s_fund_mag[j] += s_peaks[i].mag * 0.15f;
                    harm = true;
                    break;
                }
            }

            if (!harm && n_fund < MAX_PEAKS) {
                s_fund_freq[n_fund] = f;
                s_fund_mag [n_fund] = s_peaks[i].mag;
                n_fund++;
            }
        }

        // Accumulate each fundamental into its pitch class
        for (int j = 0; j < n_fund; j++) {
            float midi = 12.0f * log2f(s_fund_freq[j] / 440.0f) + 69.0f;
            int   pc   = ((int)roundf(midi) % 12 + 12) % 12;
            power[pc] += (double)s_fund_mag[j];
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
