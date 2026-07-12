#include "chroma_algo.h"
#include "chroma_fft.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// NNLS (Non-Negative Least Squares) Chroma.
//
// Approach:
//   1. Map FFT magnitudes to a 60-bin log-frequency spectrum (C2=0 .. B6=59,
//      one bin per semitone).
//   2. Build a 12-column template matrix A where column pc contains the
//      expected spectral shape of pitch class pc across all octaves and
//      harmonics, with exponentially decaying harmonic weights.
//   3. Solve: x = argmin_{x≥0} ||A x − b||² using multiplicative updates
//      (Lee-Seung NMF/NNLS rule), which for 12 variables converges in ~80
//      iterations and runs in microseconds.
//   4. Normalise x to [0,1] with a dB floor.
//
// Because the templates explicitly model the harmonic series, energy from a
// note's overtones is attributed back to the correct fundamental instead of
// leaking into neighbouring pitch classes.
// ---------------------------------------------------------------------------

static const int NNLS_N     = 8192;   // FFT window size
static const int LOG_BINS   = 60;     // C2..B6 (5 octaves × 12 semitones)
static const int N_PC       = 12;     // pitch classes
static const int NNLS_ITERS = 80;     // multiplicative update iterations

// C2 = MIDI 36 = 440 * 2^((36-69)/12) ≈ 65.406 Hz
static const float F_C2 = 65.406f;

// Map a frequency to the nearest log-frequency bin (0=C2 .. 59=B6).
// Returns -1 if out of range.
static int freq_to_logbin(float f)
{
    if (f <= 0.0f) return -1;
    float semi = 12.0f * log2f(f / F_C2);
    int   k    = (int)roundf(semi);
    return (k >= 0 && k < LOG_BINS) ? k : -1;
}

// Build the 60×12 template matrix A.
// A[k][pc] = total weighted contribution of pitch class pc to log bin k,
//            summed over octaves 2..6 and harmonics 1..10.
static void build_templates(float A[LOG_BINS][N_PC])
{
    memset(A, 0, LOG_BINS * N_PC * sizeof(float));
    const float ROLLOFF = 0.65f;   // amplitude decay per harmonic
    for (int pc = 0; pc < N_PC; pc++) {
        for (int oct = 2; oct <= 6; oct++) {
            int   midi = 12 * (oct + 1) + pc;
            float fund = 440.0f * powf(2.0f, (midi - 69) / 12.0f);
            float w    = 1.0f;
            for (int h = 1; h <= 10; h++, w *= ROLLOFF) {
                int k = freq_to_logbin(h * fund);
                if (k >= 0) A[k][pc] += w;
            }
        }
    }
}

void chroma_nnls(const float* pcm, uint64_t frame_count, uint32_t ch,
                  uint32_t sr, double t0, double t1, float result[12])
{
    memset(result, 0, N_PC * sizeof(float));
    if (!pcm || frame_count == 0 || ch == 0 || sr == 0) return;
    if (t1 - t0 > 8.0) t0 = t1 - 8.0;

    int64_t fs = (int64_t)(t0 * sr); if (fs < 0) fs = 0;
    int64_t fe = (int64_t)(t1 * sr); if (fe > (int64_t)frame_count) fe = (int64_t)frame_count;
    if (fe - fs < NNLS_N) return;

    // Template matrix and its Gram matrix (A^T A) – computed once.
    static float s_A   [LOG_BINS][N_PC];
    static float s_AtA [N_PC][N_PC];
    static bool  s_tmpl_init = false;
    if (!s_tmpl_init) {
        build_templates(s_A);
        for (int j = 0; j < N_PC; j++)
            for (int k = 0; k < N_PC; k++) {
                float v = 0.0f;
                for (int r = 0; r < LOG_BINS; r++) v += s_A[r][j] * s_A[r][k];
                s_AtA[j][k] = v;
            }
        s_tmpl_init = true;
    }

    // Hann window
    static float s_win[NNLS_N];
    static bool  s_win_init = false;
    if (!s_win_init) {
        for (int i = 0; i < NNLS_N; i++)
            s_win[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979f * i / (NNLS_N - 1)));
        s_win_init = true;
    }

    // Accumulate log-frequency power spectrum across all frames
    static float s_re[NNLS_N], s_im[NNLS_N];
    double logspec[LOG_BINS] = {};
    float freq_per_bin = (float)sr / (float)NNLS_N;
    int num_frames = 0;

    for (int64_t pos = fs; pos + NNLS_N <= fe; pos += NNLS_N) {
        for (int i = 0; i < NNLS_N; i++) {
            float s = 0.0f;
            for (uint32_t c = 0; c < ch; c++) s += pcm[(pos + i) * ch + c];
            s_re[i] = (s / (float)ch) * s_win[i];
            s_im[i] = 0.0f;
        }
        chroma_fft(s_re, s_im, NNLS_N);

        for (int b = 1; b < NNLS_N / 2; b++) {
            int k = freq_to_logbin(b * freq_per_bin);
            if (k >= 0) logspec[k] += s_re[b]*s_re[b] + s_im[b]*s_im[b];
        }
        num_frames++;
    }
    if (num_frames == 0) return;

    // Average to get the observation vector b
    float b_vec[LOG_BINS];
    for (int k = 0; k < LOG_BINS; k++)
        b_vec[k] = (float)(logspec[k] / num_frames);

    // Precompute A^T b (the numerator in the update rule)
    float Atb[N_PC] = {};
    for (int j = 0; j < N_PC; j++)
        for (int k = 0; k < LOG_BINS; k++)
            Atb[j] += s_A[k][j] * b_vec[k];

    // Multiplicative update (Lee-Seung NNLS):
    //   x ← x .* (A^T b) ./ (A^T A x + ε)
    // Initialise x from A^T b so the first iterate is non-negative.
    float x[N_PC];
    for (int j = 0; j < N_PC; j++) x[j] = Atb[j] + 1e-6f;

    for (int iter = 0; iter < NNLS_ITERS; iter++) {
        float AtAx[N_PC] = {};
        for (int j = 0; j < N_PC; j++)
            for (int k = 0; k < N_PC; k++)
                AtAx[j] += s_AtA[j][k] * x[k];
        for (int j = 0; j < N_PC; j++) {
            x[j] *= Atb[j] / (AtAx[j] + 1e-12f);
            if (x[j] < 0.0f) x[j] = 0.0f;
        }
    }

    // Normalise with dB floor
    float mx = 1e-30f;
    for (int j = 0; j < N_PC; j++) if (x[j] > mx) mx = x[j];
    for (int j = 0; j < N_PC; j++) {
        float db = 20.0f * log10f(x[j] / mx + 1e-30f);
        float v  = (db + 30.0f) / 30.0f;
        result[j] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
}
