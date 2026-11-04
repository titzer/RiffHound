#pragma once

// Microphone capture into a lock-free ring buffer, and a monophonic
// reference-tone output.  Both run on miniaudio's device threads; the UI
// thread talks to them only through atomics.

#include <string>
#include <vector>

enum ToneWave { WAVE_SINE = 0, WAVE_TRIANGLE, WAVE_SQUARE, WAVE_SAW, WAVE_COUNT };
extern const char* const TONE_WAVE_NAMES[WAVE_COUNT];

bool audio_init();
void audio_shutdown();

// Capture.  device_name empty = system default.  Returns false (and leaves
// capture stopped) if the device cannot be opened; audio_error() says why.
bool        capture_start(const std::string& device_name);
void        capture_stop();
bool        capture_running();
float       capture_sample_rate();
// Copy the newest n samples (oldest first) into out.  Returns false until
// at least n samples have been captured since the last start.
bool        capture_latest(float* out, int n);
std::vector<std::string> capture_device_names();   // refreshed on each call
const char* audio_error();

// Reference tone.  freq 0 fades the tone out.
void tone_set(float freq, int wave, float volume);
