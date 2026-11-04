#pragma once

// Monophonic pitch detection (YIN) and the display tracker that turns raw
// per-frame estimates into a steady reading.  No UI or audio-device code
// here, so the self-test can drive it with synthetic signals.

#include <vector>

struct YinResult {
    float freq       = 0.0f;   // Hz, 0 = no pitch
    float confidence = 0.0f;   // 1 - CMND at the chosen lag, 0..1
    float rms        = 0.0f;   // input level (linear, after mean removal)
};

class Yin {
public:
    // buffer: n mono samples, oldest first.
    YinResult detect(const float* buffer, int n, float sample_rate,
                     float min_freq, float max_freq,
                     float threshold, float gate_rms);
private:
    std::vector<float>  x_;      // mean-removed copy of the input
    std::vector<double> energy_; // prefix sums of x^2
    std::vector<float>  diff_;
    std::vector<float>  cmnd_;
};

struct TrackerParams {
    float min_confidence = 0.55f;
    float smoothing      = 0.30f;  // per-60fps-frame EMA weight on the old value
    float hold_ms        = 250.0f; // keep the last reading this long through dropouts
};

// Smooths YIN output in the pitch (semitone) domain.  A jump of more than
// a semitone and a half resets the filter so note changes snap instead of
// gliding.
class PitchTracker {
public:
    // Returns the display frequency (0 = nothing to show).
    float update(const YinResult& r, double now_s, const TrackerParams& p);
    void  reset() { smoothed_midi_ = 0.0; last_good_t_ = -1e9; }
private:
    double smoothed_midi_ = 0.0;   // relative to A4 = 0, in semitones; 0 = unset
    double last_good_t_   = -1e9;
    double last_t_        = 0.0;
    float  last_freq_     = 0.0f;
};

// Decides which note is "active" (named in the readout, lit on the piano,
// and the reference for the cents reading).  The continuous pitch can flick
// across a semitone boundary or carry a one-frame octave error; the active
// note changes only when a different note has been heard with high
// confidence for settle_ms, and the current note is kept until the pitch is
// more than 50 + hysteresis cents away from it.
struct NoteParams {
    float confidence   = 0.80f;   // per-frame YIN confidence needed to (re)activate a note
    float settle_ms    = 60.0f;
    float hysteresis   = 15.0f;   // cents beyond the semitone midpoint
};

class NoteTracker {
public:
    // midi: continuous pitch (0 = none); confidence: this frame's raw YIN
    // confidence.  Returns the active note, -1 = none.
    int  update(float midi, float confidence, double now_s, const NoteParams& p);
    void reset() { note_ = cand_ = -1; }
    int  note() const { return note_; }
private:
    int    note_ = -1;
    int    cand_ = -1;
    double cand_t_ = 0.0;
};

// Note naming / conversion.
float       midi_to_freq(float midi, float a4);
float       freq_to_midi(float freq, float a4);
const char* pitch_class_name(int midi);     // "C", "C#", ...
int         midi_octave(int midi);          // scientific pitch octave (C4 = 60)
bool        midi_is_black(int midi);

// Headless check of the detector on synthetic tones; prints a table and
// returns the number of failures.
int pitch_selftest();
