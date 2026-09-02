#include "chroma_algo.h"
#include "chroma_fft.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Whitened log-frequency chroma (Cho & Bello 2014 / Mauch 2010 front end).
//
// The chord-recognition literature's consistent finding is that the front
// end, not the classifier, decides accuracy, and that three steps carry most
// of it:
//   1. a log-frequency (constant-Q-like) spectrum, 3 bins per semitone,
//      with a global tuning estimate from the sub-semitone bins;
//   2. log compression of magnitudes (quiet harmonic content matters as much
//      as loud fundamentals);
//   3. octave-wide running-mean subtraction and standard-deviation
//      normalisation along the frequency axis ("spectral whitening") --
//      broadband drum energy and timbral slope are removed per frame, which
//      is worth more than any decoder tweak.
// The whitened spectrum is folded into 12 pitch classes with a raised-cosine
// octave weighting centred on the octaves chords actually live in.
// ---------------------------------------------------------------------------

static const int WL_N    = 8192;          // FFT size
static const int WL_HOP  = 4096;
static const int WL_BINS = WL_N / 2;

static const int   WL_BPS   = 3;                    // log-freq bins per semitone
static const float WL_MIDI0 = 24.0f;                // C1 (~32.7 Hz)
static const int   WL_SEMIS = 84;                   // C1..B7
static const int   WL_LF    = WL_SEMIS * WL_BPS;    // log-freq bins
static const int   WL_HALF  = 18;                   // whitening half-window (1 octave)

void chroma_whitened(const float* pcm, uint64_t frame_count, uint32_t ch,
                     uint32_t sr, double t0, double t1, float result[12])
{
    memset(result, 0, 12 * sizeof(float));
    if (!pcm || frame_count == 0 || ch == 0 || sr == 0) return;
    if (t1 - t0 > 8.0) t0 = t1 - 8.0;

    int64_t fs = (int64_t)(t0 * sr); if (fs < 0) fs = 0;
    int64_t fe = (int64_t)(t1 * sr); if (fe > (int64_t)frame_count) fe = (int64_t)frame_count;
    if (fe - fs < WL_N / 2) return;

    static float s_re[WL_N], s_im[WL_N], s_win[WL_N];
    static float s_mag[WL_BINS];
    static float s_lf[WL_LF], s_wh[WL_LF], s_acc[WL_LF];
    static float s_hamm[2 * WL_HALF + 1];
    static bool  s_init = false;
    if (!s_init) {
        for (int i = 0; i < WL_N; i++)
            s_win[i] = 0.54f - 0.46f * cosf(2.0f * 3.14159265f * i / (WL_N - 1));
        for (int i = 0; i <= 2 * WL_HALF; i++)
            s_hamm[i] = 0.54f - 0.46f * cosf(2.0f * 3.14159265f * i / (2.0f * WL_HALF));
        s_init = true;
    }

    const float fpb = (float)sr / (float)WL_N;      // Hz per FFT bin
    memset(s_acc, 0, sizeof(s_acc));
    double tune_re = 0.0, tune_im = 0.0;            // sub-semitone energy phase
    int    n_frames = 0;

    // Short intervals still get one frame: start it at fs even if it needs
    // zero-padding at the end.
    for (int64_t pos = fs; pos < fe - WL_N / 4 && n_frames < 64; pos += WL_HOP) {
        for (int i = 0; i < WL_N; i++) {
            int64_t fi = pos + i;
            float s = 0.0f;
            if (fi < fe)
                for (uint32_t c = 0; c < ch; c++) s += pcm[fi * ch + c];
            s_re[i] = (s / (float)ch) * s_win[i];
            s_im[i] = 0.0f;
        }
        chroma_fft(s_re, s_im, WL_N);
        float mmax = 1e-30f;
        for (int k = 0; k < WL_BINS; k++) {
            s_mag[k] = sqrtf(s_re[k] * s_re[k] + s_im[k] * s_im[k]);
            if (s_mag[k] > mmax) mmax = s_mag[k];
        }

        // Log-frequency mapping (sampled by linear interpolation) with
        // log compression relative to the frame's maximum.
        for (int b = 0; b < WL_LF; b++) {
            float midi = WL_MIDI0 + (float)b / WL_BPS;
            float f    = 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
            float x    = f / fpb;
            int   k    = (int)x;
            float v    = 0.0f;
            if (k >= 1 && k + 1 < WL_BINS) {
                float fr = x - k;
                v = s_mag[k] * (1.0f - fr) + s_mag[k + 1] * fr;
            }
            s_lf[b] = log1pf(100.0f * v / mmax);
        }

        // Tuning evidence: distribution of energy across the 3 sub-semitone
        // positions, accumulated as a phase over all frames.
        for (int b = 0; b < WL_LF; b++) {
            int sub = b % WL_BPS;
            float ang = 2.0f * 3.14159265f * sub / WL_BPS;
            tune_re += s_lf[b] * cosf(ang);
            tune_im += s_lf[b] * sinf(ang);
        }

        // Whitening along the log-frequency axis: Hamming-weighted running
        // mean subtraction (clamped at zero) and std division, one octave
        // wide.
        for (int b = 0; b < WL_LF; b++) {
            double wsum = 0.0, mu = 0.0;
            for (int d = -WL_HALF; d <= WL_HALF; d++) {
                int j = b + d;
                if (j < 0 || j >= WL_LF) continue;
                double w = s_hamm[d + WL_HALF];
                mu += w * s_lf[j]; wsum += w;
            }
            mu = wsum > 0 ? mu / wsum : 0.0;
            double var = 0.0;
            for (int d = -WL_HALF; d <= WL_HALF; d++) {
                int j = b + d;
                if (j < 0 || j >= WL_LF) continue;
                double w = s_hamm[d + WL_HALF];
                double dd = s_lf[j] - mu;
                var += w * dd * dd;
            }
            var = wsum > 0 ? var / wsum : 0.0;
            double sd = sqrt(var) + 1e-3;
            double v  = (s_lf[b] - mu) / sd;
            s_wh[b] = v > 0.0 ? (float)v : 0.0f;
        }
        for (int b = 0; b < WL_LF; b++) s_acc[b] += s_wh[b];
        n_frames++;
    }
    if (n_frames == 0) return;

    // Global tuning offset in sub-semitone bins: [-0.5, 0.5) of a bin group.
    float tune = 0.0f;
    if (tune_re != 0.0 || tune_im != 0.0) {
        float theta = atan2f((float)tune_im, (float)tune_re);
        tune = theta / (2.0f * 3.14159265f) * WL_BPS;   // in log-freq bins
        if (tune > 1.5f)  tune -= 3.0f;
        if (tune < -1.5f) tune += 3.0f;
    }

    // Fold to pitch classes, sampling the accumulated whitened spectrum at
    // tuning-corrected semitone centres, weighted toward the octaves chords
    // live in (C2..C6, peak near C4).
    double power[12] = {};
    for (int s = 0; s < WL_SEMIS; s++) {
        float x = s * WL_BPS + 1 + tune;     // +1: centre sub-bin of the semitone
        int   b = (int)floorf(x);
        float fr = x - b;
        if (b < 0 || b + 1 >= WL_LF) continue;
        float v = s_acc[b] * (1.0f - fr) + s_acc[b + 1] * fr;
        float midi = WL_MIDI0 + s;
        float oct  = (midi - 60.0f) / 30.0f;             // C4-centred
        float w    = 0.5f * (1.0f + cosf(3.14159265f * (oct < -1 ? -1 : oct > 1 ? 1 : oct)));
        w = 0.15f + 0.85f * w;
        int pc = ((int)midi) % 12;
        power[pc] += (double)v * w;
    }
    double mx = 1e-30;
    for (int i = 0; i < 12; i++) if (power[i] > mx) mx = power[i];
    for (int i = 0; i < 12; i++) {
        float v = (float)(power[i] / mx);
        result[i] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
}
