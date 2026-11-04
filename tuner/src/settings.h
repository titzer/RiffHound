#pragma once

#include <string>

struct Settings {
    // Detection
    float a4             = 440.0f;
    float yin_threshold  = 0.15f;
    float min_confidence = 0.55f;
    float smoothing      = 0.30f;
    float min_freq       = 70.0f;
    float max_freq       = 900.0f;
    int   window_size    = 4096;     // analysis window, samples
    float hold_ms        = 250.0f;
    float gate_db        = -60.0f;   // input below this RMS (dBFS) reads as silence
    float note_confidence = 0.80f;   // stricter gate for changing the active note
    float note_settle_ms = 60.0f;
    float note_hysteresis = 15.0f;   // cents

    // Display
    float in_tune_cents  = 5.0f;     // green
    float close_cents    = 15.0f;    // yellow; beyond is red
    float history_s      = 8.0f;
    bool  show_history   = true;
    bool  show_key_labels = true;
    bool  always_on_top  = false;
    int   octave_shift   = 0;        // keyboard shows C(2+shift) .. C(5+shift)

    // Reference tone
    int   tone_wave      = 0;
    float tone_volume    = 0.25f;

    // Input
    std::string input_device;        // empty = system default

    // Window
    int win_x = -1, win_y = -1, win_w = 520, win_h = 400;
};

void settings_load(Settings* s);
void settings_save(const Settings& s);
void settings_clamp(Settings* s);
