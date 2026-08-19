#include "chroma_algo.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Resonate chroma (Alexandre Francois, https://alexandrefrancois.org/Resonate/)
//
// Instead of transforming blocks of samples into a spectrum, a bank of complex
// resonators is updated once per input sample.  Each resonator k is tuned to a
// fixed frequency f_k and carries three pieces of state:
//
//   P_k  unit phasor, rotated by a constant angle every sample:
//            P_k(t) = P_k(t-dt) * e^(-i w_k dt),      w_k = 2 pi f_k
//   R_k  exponentially-weighted moving average of the demodulated signal:
//            R_k(t) = (1 - a_k) R_k(t-dt) + a_k x(t) P_k(t)
//   S_k  a second EWMA that smooths R_k's residual power/phase ripple:
//            S_k(t) = (1 - b_k) S_k(t-dt) + b_k R_k(t)
//
// R_k is a sliding-window DFT bin with an exponential rather than rectangular
// window, so there is no frame size, no hop size and no window function: the
// magnitude |S_k| is a running estimate available at every sample.  Cost is
// O(resonators) per sample and O(resonators) memory, independent of how long
// the signal is.
//
// For x(t) = A cos(w_k t) the product x(t) P_k(t) is (A/2)(1 + e^(-2 i w_k t));
// the EWMA keeps the DC term and attenuates the 2 w_k ripple, so |R_k| -> A/2
// at every frequency.  The bank therefore needs no per-bin normalisation.
//
// a_k sets each resonator's bandwidth, and is the one place this deviates from
// the paper.  The paper's general-audio heuristic is
//
//       a_k = 1 - e^(-dt * f_k / ln(1 + f_k))
//
// which works out to a half-bandwidth of about half a semitone: fine for a
// broad 20 Hz .. 20 kHz analysis, but for chroma it lets each note bleed most
// of its energy into its neighbouring pitch classes.  Measured on a synthetic
// bench (single notes and triads, separation between sounding and silent pitch
// classes), that heuristic scores 0.58 where a narrower bank scores 0.82.
//
// So a_k is instead derived from the resolution chroma actually needs -- a
// fixed fraction of a semitone -- which keeps the bank constant-Q:
//
//       a_k = 1 - e^(-2 pi dt * BW_SEMITONES * (2^(1/12) - 1) * f_k)
//
// The resulting time constant 1/a_k still shrinks as frequency rises, exactly
// as the paper intends: ~273 ms at C2 (65 Hz), ~41 ms at A4 (440 Hz).  Low
// notes get the long integration they need to resolve semitones only 3.9 Hz
// apart, while high notes stay responsive.  This is where the resonator bank
// beats a fixed-size transform: a 4096-sample Goertzel frame gives every note
// the same 10.8 Hz resolution, which simply cannot separate semitones in the
// bass.
//
// The bank is tuned to equal-tempered semitones C2..B6 (60 resonators).  We
// deliberately use the fixed-frequency model rather than the frequency-tracking
// extension: the whole point here is to measure energy *at* the tempered pitch
// grid, and self-tuning resonators would drift off it (and potentially off
// their own pitch class) on vibrato or a detuned instrument.
//
// The chroma window is handled by averaging each resonator's instantaneous
// power over the window rather than sampling it at the end, so the result
// reflects the whole span the caller asked about, not just its last few ms.
// ---------------------------------------------------------------------------

static const int OCT_LO   = 2;                              // lowest octave
static const int OCT_HI   = 6;                              // highest octave
static const int N_OCT    = OCT_HI - OCT_LO + 1;            // 5
static const int N_PC     = 12;                             // pitch classes
static const int N_RES    = N_PC * N_OCT;                   // 60 resonators

// Resonator half-bandwidth as a fraction of a semitone.  Narrower is more
// selective but takes longer to settle; the bench flattens out below ~0.15
// while short windows keep getting worse, so this sits at the knee.
static const float BW_SEMITONES = 0.15f;

// b_k = BETA_SCALE * a_k.  The smoother is a second pole on the same signal,
// so it also steepens the skirts -- most of this algorithm's pitch-class
// separation comes from having it.  0.5 (twice the resonator's time constant)
// measured slightly better than matching it.
static const float BETA_SCALE = 0.5f;

// The phasor is advanced by repeated complex multiplication, which slowly
// bleeds magnitude in single precision; rescale it back to the unit circle
// this often.  Must be a power of two (the loop tests with a mask).
static const int RENORM_EVERY = 512;

// Skip this many time constants at the head of the window before believing a
// resonator's output, so the EWMA start-up transient does not count as signal.
// (Rescaling the transient by the cascade's step response instead of skipping
// it was tried and measured worse: an unsettled resonator is also a less
// frequency-selective one, so restoring those samples amplifies leakage.)
static const float WARMUP_TAU = 3.0f;

// Largest span processed, in samples (~8 s at 48 kHz, matching the other
// algorithms' 8 s cap).  Longer requests keep the most recent samples.
static const int64_t MAX_SAMPLES = 384000;

static float s_mono[MAX_SAMPLES];

// Resonator tuning table, built once: frequency and EWMA rate per resonator.
// a_k depends on the sample rate, so the table is rebuilt if that changes.
static float    s_freq[N_RES];
static float    s_alpha[N_RES];
static uint32_t s_tuned_sr = 0;

static void ensure_tuning(uint32_t sr)
{
    if (s_tuned_sr == sr) return;
    const float dt   = 1.0f / (float)sr;
    // Half-bandwidth in Hz is BW_SEMITONES semitones above the resonator's own
    // frequency, i.e. proportional to f: a constant-Q bank.
    const float semi = powf(2.0f, 1.0f / 12.0f) - 1.0f;
    for (int oi = 0; oi < N_OCT; oi++)
        for (int pc = 0; pc < N_PC; pc++) {
            int   midi = 12 * (OCT_LO + oi + 1) + pc;       // C4 = 60, A4 = 69
            float f    = 440.0f * powf(2.0f, (midi - 69) / 12.0f);
            int   r    = oi * N_PC + pc;
            s_freq[r]  = f;
            s_alpha[r] = 1.0f - expf(-6.283185307179586f * BW_SEMITONES * semi * f * dt);
        }
    s_tuned_sr = sr;
}

void chroma_resonate(const float* pcm, uint64_t frame_count, uint32_t ch,
                      uint32_t sr, double t0, double t1, float result[12])
{
    memset(result, 0, N_PC * sizeof(float));
    if (!pcm || frame_count == 0 || ch == 0 || sr == 0) return;

    int64_t fs = (int64_t)(t0 * sr); if (fs < 0) fs = 0;
    int64_t fe = (int64_t)(t1 * sr); if (fe > (int64_t)frame_count) fe = (int64_t)frame_count;
    if (fe - fs > MAX_SAMPLES) fs = fe - MAX_SAMPLES;   // keep the tail
    int64_t n_samp = fe - fs;
    if (n_samp <= 0) return;

    ensure_tuning(sr);

    // Materialise the window as mono up front.  The resonator loop is then
    // resonator-outer / sample-inner, which keeps each resonator's whole state
    // in registers for the length of the window.
    const float inv_ch = 1.0f / (float)ch;
    for (int64_t i = 0; i < n_samp; i++) {
        float s = 0.0f;
        for (uint32_t c = 0; c < ch; c++) s += pcm[(fs + i) * ch + c];
        s_mono[i] = s * inv_ch;
    }

    const float two_pi   = 6.283185307179586f;
    const float nyq_lim  = 0.45f * (float)sr;    // ignore resonators too near Nyquist
    double power[N_PC] = {};

    for (int r = 0; r < N_RES; r++) {
        if (s_freq[r] >= nyq_lim) continue;

        const float a = s_alpha[r];
        const float b = a * BETA_SCALE;

        // Ignore the EWMA start-up transient.  Never skip more than half the
        // window, or a short window would yield no samples at all.
        int64_t skip = (int64_t)(WARMUP_TAU / a);
        if (skip > n_samp / 2) skip = n_samp / 2;

        // Constant per-sample rotation e^(-i w dt), w dt = 2 pi f / sr.
        const float th = two_pi * s_freq[r] / (float)sr;
        const float cw = cosf(th), sw = -sinf(th);

        float pr = 1.0f, pi = 0.0f;    // phasor P
        float rr = 0.0f, ri = 0.0f;    // resonator R
        float qr = 0.0f, qi = 0.0f;    // smoothed S
        double acc = 0.0;
        int64_t nacc = 0;

        for (int64_t n = 0; n < n_samp; n++) {
            // P *= e^(-i w dt)
            float np = pr * cw - pi * sw;
            pi       = pr * sw + pi * cw;
            pr       = np;
            if ((n & (RENORM_EVERY - 1)) == 0) {
                float g = 1.0f / sqrtf(pr * pr + pi * pi);
                pr *= g; pi *= g;
            }

            const float x = s_mono[n];
            rr += a * (x * pr - rr);      // R += a (x P - R)
            ri += a * (x * pi - ri);
            qr += b * (rr - qr);          // S += b (R - S)
            qi += b * (ri - qi);

            if (n >= skip) { acc += (double)(qr * qr + qi * qi); nacc++; }
        }

        if (nacc) power[r % N_PC] += acc / (double)nacc;
    }

    // Normalise: peak -> 0 dB, -30 dB floor -> 0 (same mapping as the other
    // algorithms so the panel's colour scale stays comparable).
    double mx = 1e-30;
    for (int i = 0; i < N_PC; i++) if (power[i] > mx) mx = power[i];
    for (int i = 0; i < N_PC; i++) {
        float db = 10.0f * log10f((float)(power[i] / mx) + 1e-30f);
        float v  = (db + 30.0f) / 30.0f;
        result[i] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
}
