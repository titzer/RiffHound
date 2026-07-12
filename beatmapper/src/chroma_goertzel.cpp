#include "chroma_algo.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Goertzel chroma algorithms (Hann and Blackman-Harris window variants).
//
// The Goertzel algorithm evaluates the DFT at any arbitrary frequency,
// avoiding the bin-quantisation error of a standard FFT.  Analysis runs
// over non-overlapping frames of GOERTZEL_N samples; each frame contributes
// independently to the 12-element pitch-class energy accumulator.
//
// Target frequencies: pitch classes C..B in octaves 2..6 (60 targets total).
// ---------------------------------------------------------------------------

static const int GOERTZEL_N = 4096;   // samples per analysis frame

// Pitch-class target frequencies – computed once on first call.
static float s_target[12][5];         // [pitch_class][octave_index], octaves 2..6
static bool  s_target_init = false;

static void ensure_targets()
{
    if (s_target_init) return;
    for (int pc = 0; pc < 12; pc++)
        for (int oi = 0; oi < 5; oi++) {
            int oct  = oi + 2;                     // octaves 2, 3, 4, 5, 6
            int midi = 12 * (oct + 1) + pc;        // C4=60, A4=69 convention
            s_target[pc][oi] = 440.0f * powf(2.0f, (midi - 69) / 12.0f);
        }
    s_target_init = true;
}

// Single-bin Goertzel filter applied to a Hann- (or other-) windowed mono block.
static float goertzel(const float* mono, const float* win, int N,
                       float freq, float sr)
{
    float w     = 2.0f * 3.14159265358979f * freq / sr;
    float coeff = 2.0f * cosf(w);
    float s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < N; i++) {
        float s0 = mono[i] * win[i] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1*s1 + s2*s2 - coeff*s1*s2;
}

// Common inner loop; win[] selects the window function.
static void run_goertzel(const float* pcm, uint64_t frame_count, uint32_t ch,
                          uint32_t sr, double t0, double t1,
                          float result[12], const float* win)
{
    memset(result, 0, 12 * sizeof(float));
    ensure_targets();
    if (!pcm || frame_count == 0 || ch == 0 || sr == 0) return;
    if (t1 - t0 > 8.0) t0 = t1 - 8.0;

    int64_t fs = (int64_t)(t0 * sr); if (fs < 0) fs = 0;
    int64_t fe = (int64_t)(t1 * sr); if (fe > (int64_t)frame_count) fe = (int64_t)frame_count;
    if (fe - fs < GOERTZEL_N) return;

    static float s_mono[GOERTZEL_N];
    double power[12] = {};

    for (int64_t pos = fs; pos + GOERTZEL_N <= fe; pos += GOERTZEL_N) {
        // Stereo → mono
        for (int i = 0; i < GOERTZEL_N; i++) {
            float s = 0.0f;
            for (uint32_t c = 0; c < ch; c++) s += pcm[(pos + i) * ch + c];
            s_mono[i] = s / (float)ch;
        }
        // One Goertzel filter per target note; accumulate into pitch class.
        for (int pc = 0; pc < 12; pc++)
            for (int oi = 0; oi < 5; oi++)
                power[pc] += goertzel(s_mono, win, GOERTZEL_N,
                                      s_target[pc][oi], (float)sr);
    }

    // Normalise: peak → 0 dB, −30 dB floor → 0.
    double mx = 1e-30;
    for (int i = 0; i < 12; i++) if (power[i] > mx) mx = power[i];
    for (int i = 0; i < 12; i++) {
        float db = 10.0f * log10f((float)(power[i] / mx) + 1e-30f);
        float v  = (db + 30.0f) / 30.0f;
        result[i] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Hann window: w[n] = 0.5 * (1 − cos(2π n/(N−1)))
void chroma_goertzel_hann(const float* pcm, uint64_t frames, uint32_t ch,
                           uint32_t sr, double t0, double t1, float result[12])
{
    static float s_win[GOERTZEL_N];
    static bool  s_init = false;
    if (!s_init) {
        for (int i = 0; i < GOERTZEL_N; i++)
            s_win[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979f * i / (GOERTZEL_N - 1)));
        s_init = true;
    }
    run_goertzel(pcm, frames, ch, sr, t0, t1, result, s_win);
}

// 4-term Blackman-Harris: −92 dB sidelobe level (vs −31 dB for Hann).
// Coefficients from Harris (1978): a0=0.35875, a1=0.48829, a2=0.14128, a3=0.01168.
void chroma_goertzel_blackman(const float* pcm, uint64_t frames, uint32_t ch,
                               uint32_t sr, double t0, double t1, float result[12])
{
    static float s_win[GOERTZEL_N];
    static bool  s_init = false;
    if (!s_init) {
        for (int i = 0; i < GOERTZEL_N; i++) {
            float t = 2.0f * 3.14159265358979f * i / (GOERTZEL_N - 1);
            s_win[i] = 0.35875f
                     - 0.48829f * cosf(t)
                     + 0.14128f * cosf(2.0f * t)
                     - 0.01168f * cosf(3.0f * t);
        }
        s_init = true;
    }
    run_goertzel(pcm, frames, ch, sr, t0, t1, result, s_win);
}
