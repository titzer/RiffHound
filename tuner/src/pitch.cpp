#include "pitch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>

static const char* NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

float midi_to_freq(float midi, float a4) { return a4 * powf(2.0f, (midi - 69.0f) / 12.0f); }
float freq_to_midi(float freq, float a4) { return 69.0f + 12.0f * log2f(freq / a4); }
const char* pitch_class_name(int midi)   { return NOTE_NAMES[((midi % 12) + 12) % 12]; }
int  midi_octave(int midi)               { return (int)floor(midi / 12.0) - 1; }
bool midi_is_black(int midi) {
    int pc = ((midi % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

// Dot product with independent partial sums so the compiler can vectorize
// it without -ffast-math.
static inline double dot(const float* a, const float* b, int n) {
    float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; k++) s[k] += a[i + k] * b[i + k];
    double t = 0.0;
    for (int k = 0; k < 8; k++) t += s[k];
    for (; i < n; i++) t += (double)a[i] * b[i];
    return t;
}

YinResult Yin::detect(const float* buf, int n, float sr,
                      float min_freq, float max_freq,
                      float threshold, float gate_rms) {
    YinResult res;
    if (n < 64 || sr <= 0) return res;

    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += buf[i];
    mean /= n;
    double ss = 0.0;
    for (int i = 0; i < n; i++) { double d = buf[i] - mean; ss += d * d; }
    res.rms = (float)sqrt(ss / n);
    if (res.rms < gate_rms) return res;

    // No window function: the web prototype applied a Hann window here,
    // which tilts the difference function and reads low notes sharp (+20
    // cents at 73 Hz).  The difference function needs none.
    x_.resize(n);
    for (int i = 0; i < n; i++) x_[i] = (float)(buf[i] - mean);

    int min_tau = (int)floorf(sr / max_freq);
    int max_tau = (int)ceilf(sr / min_freq);
    if (min_tau < 2) min_tau = 2;
    if (max_tau > n / 2) max_tau = n / 2;   // keep at least half the window integrated
    if (max_tau <= min_tau + 1) return res;

    // d(tau) = sum_{i<n-tau} (x[i] - x[i+tau])^2
    //        = E[0, n-tau) + E[tau, n) - 2 * sum x[i] x[i+tau]
    energy_.resize(n + 1);
    energy_[0] = 0.0;
    for (int i = 0; i < n; i++) energy_[i + 1] = energy_[i] + (double)x_[i] * x_[i];

    cmnd_.assign(max_tau + 2, 1.0f);
    diff_.assign(max_tau + 2, 0.0f);
    double running = 0.0;
    const float* x = x_.data();
    for (int tau = 1; tau <= max_tau + 1 && tau < n; tau++) {
        int len = n - tau;
        double e1 = energy_[len];
        double e2 = energy_[n] - energy_[tau];
        double d  = e1 + e2 - 2.0 * dot(x, x + tau, len);
        if (d < 0.0) d = 0.0;
        diff_[tau] = (float)d;
        running += d;
        cmnd_[tau] = running > 0.0 ? (float)(d * tau / running) : 1.0f;
    }

    int est = 0;
    for (int tau = min_tau; tau <= max_tau; tau++) {
        if (cmnd_[tau] < threshold) {
            while (tau + 1 <= max_tau && cmnd_[tau + 1] < cmnd_[tau]) tau++;
            est = tau;
            break;
        }
    }
    if (!est) {
        // No dip under the threshold: take the global minimum if it is at
        // least moderately periodic.
        int best = min_tau;
        float bv = 1e9f;
        for (int tau = min_tau; tau <= max_tau; tau++)
            if (cmnd_[tau] < bv) { bv = cmnd_[tau]; best = tau; }
        if (bv < 0.45f) est = best;
    }
    if (!est) return res;

    double better = est;
    if (est > 1 && est < max_tau + 1) {
        // Parabolic refinement on the raw difference function (as in the
        // YIN paper); the CMND's tau/running factor would skew the vertex.
        double s0 = diff_[est - 1], s1 = diff_[est], s2 = diff_[est + 1];
        double denom = 2.0 * (2.0 * s1 - s2 - s0);
        if (fabs(denom) > 1e-9) {
            double off = (s2 - s0) / denom;
            if (off > -1.0 && off < 1.0) better = est + off;
        }
    }
    float conf = 1.0f - cmnd_[est];
    res.confidence = conf < 0 ? 0 : conf > 1 ? 1 : conf;
    res.freq = (float)(sr / better);
    return res;
}

float PitchTracker::update(const YinResult& r, double now, const TrackerParams& p) {
    double dt = now - last_t_;
    last_t_ = now;
    if (dt < 0 || dt > 0.5) dt = 1.0 / 60.0;

    if (r.freq > 0 && r.confidence >= p.min_confidence) {
        double m = 12.0 * log2(r.freq / 440.0);   // semitones from A440, any reference works
        if (smoothed_midi_ == 0.0 || fabs(m - smoothed_midi_) > 1.5) {
            smoothed_midi_ = m;
        } else {
            // The prototype applied alpha once per 60 Hz frame; scale it so
            // the time constant does not depend on the frame rate.
            double a = pow((double)p.smoothing, dt * 60.0);
            smoothed_midi_ = a * smoothed_midi_ + (1.0 - a) * m;
        }
        if (smoothed_midi_ == 0.0) smoothed_midi_ = 1e-9;
        last_good_t_ = now;
        last_freq_ = (float)(440.0 * pow(2.0, smoothed_midi_ / 12.0));
        return last_freq_;
    }
    if (now - last_good_t_ < p.hold_ms / 1000.0) return last_freq_;
    smoothed_midi_ = 0.0;
    return 0.0f;
}

int NoteTracker::update(float midi, float conf, double now, const NoteParams& p) {
    if (midi <= 0) { note_ = cand_ = -1; return note_; }
    if (note_ >= 0 && fabsf(midi - note_) <= 0.5f + p.hysteresis / 100.0f) {
        cand_ = -1;                       // still within the current note's band
        return note_;
    }
    if (conf < p.confidence) { cand_ = -1; return note_; }   // not sure enough to change
    int n = (int)lroundf(midi);
    if (n != cand_) { cand_ = n; cand_t_ = now; }
    if ((now - cand_t_) * 1000.0 >= p.settle_ms) { note_ = n; cand_ = -1; }
    return note_;
}

// ---------------------------------------------------------------------------
// Self-test

static void synth(std::vector<float>& out, int n, float sr, float f0, int kind,
                  float noise, unsigned* seed) {
    out.resize(n);
    for (int i = 0; i < n; i++) {
        double t = i / (double)sr;
        double v = 0.0;
        switch (kind) {
        case 0: v = sin(2 * M_PI * f0 * t); break;                         // sine
        case 1:                                                            // voice-ish: harmonics, weak fundamental
            v = 0.35 * sin(2 * M_PI * f0 * t) + 0.8 * sin(2 * M_PI * 2 * f0 * t + 0.3) +
                0.5 * sin(2 * M_PI * 3 * f0 * t + 1.1) + 0.3 * sin(2 * M_PI * 4 * f0 * t + 2.0);
            v *= 0.5; break;
        case 2: {                                                          // sawtooth (string-ish)
            double ph = f0 * t - floor(f0 * t);
            v = 2.0 * ph - 1.0; v *= 0.5; break;
        }
        }
        *seed = *seed * 1664525u + 1013904223u;
        double nz = ((*seed >> 8) / 16777216.0) * 2.0 - 1.0;
        out[i] = (float)(0.3 * v + noise * nz);
    }
}

int pitch_selftest() {
    const float SR = 48000.0f;
    const int   N  = 4096;
    const char* KIND[] = { "sine", "voice", "saw" };
    const float FREQS[] = { 73.42f, 82.41f, 110.0f, 196.0f, 261.63f, 329.63f, 440.0f, 659.25f, 880.0f };
    Yin yin;
    std::vector<float> buf;
    unsigned seed = 12345;
    int fails = 0, total = 0;
    double worst = 0.0;
    printf("%-6s %8s %9s %8s %6s\n", "kind", "f0", "detected", "cents", "conf");
    for (int k = 0; k < 3; k++) {
        for (float f0 : FREQS) {
            synth(buf, N, SR, f0, k, 0.01f, &seed);
            YinResult r = yin.detect(buf.data(), N, SR, 60.0f, 1000.0f, 0.15f, 0.001f);
            double cents = r.freq > 0 ? 1200.0 * log2(r.freq / f0) : 9999.0;
            bool ok = fabs(cents) < 3.0;
            if (fabs(cents) > worst && ok) worst = fabs(cents);
            total++;
            if (!ok) fails++;
            printf("%-6s %8.2f %9.2f %+8.2f %6.2f %s\n", KIND[k], f0, r.freq, cents,
                   r.confidence, ok ? "" : "FAIL");
        }
    }
    // Silence must read as no pitch.
    std::vector<float> quiet(N, 0.0f);
    YinResult r = yin.detect(quiet.data(), N, SR, 60.0f, 1000.0f, 0.15f, 0.001f);
    total++;
    if (r.freq != 0.0f) { fails++; printf("silence -> %.2f Hz FAIL\n", r.freq); }

    // Note gate: vibrato straddling the C/C# boundary must not flicker, a
    // one-frame octave error must not register, a real change must.
    {
        NoteTracker nt;
        NoteParams np;
        double tt = 0.0;
        int changes = 0, prev = -1;
        auto step = [&](float m, float c) {
            int n = nt.update(m, c, tt, np);
            if (n != prev) { changes++; prev = n; }
            tt += 1.0 / 60.0;
        };
        for (int i = 0; i < 120; i++) step(60.3f, 0.95f);                       // settle on C4
        for (int i = 0; i < 240; i++) step(60.5f + 0.12f * sinf(i * 0.4f), 0.9f);  // +/-12c around the boundary
        step(72.3f, 0.9f);                                                       // single octave blip
        for (int i = 0; i < 30; i++) step(60.4f, 0.9f);
        for (int i = 0; i < 30; i++) step(62.4f, 0.5f);                          // low confidence: ignored
        bool held = prev == 60;
        for (int i = 0; i < 30; i++) step(62.1f, 0.9f);                          // real change to D4
        total++;
        bool ok = held && prev == 62 && changes == 2;
        if (!ok) fails++;
        printf("note gate: %d changes, held=%d, final=%d %s\n", changes, held, prev, ok ? "" : "FAIL");
    }

    // Timing: what one analysis frame costs at the default settings.
    synth(buf, N, SR, 220.0f, 1, 0.01f, &seed);
    auto t0 = std::chrono::steady_clock::now();
    const int REPS = 200;
    for (int i = 0; i < REPS; i++)
        yin.detect(buf.data(), N, SR, 70.0f, 900.0f, 0.15f, 0.001f);
    double us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - t0).count() / REPS;

    printf("%d/%d passed, worst passing error %.2f cents, %.0f us per %d-sample frame\n",
           total - fails, total, worst, us, N);
    return fails;
}
