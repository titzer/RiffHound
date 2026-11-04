#pragma once

// State shared between the main loop (audio analysis, window) and the UI.

#include <deque>
#include <vector>
#include "pitch.h"
#include "settings.h"

struct GLFWwindow;
struct ImFont;

struct HistPoint {
    double t;
    float  midi;      // continuous MIDI pitch, 0 = no pitch this frame
    float  err;       // uncertainty half-width in cents (0 when midi == 0)
    int    ref_note;  // reference tone sounding, -1 = none
};

struct Tuner {
    Settings s;

    // Live analysis
    bool      mic_on       = false;
    YinResult raw;
    float     level_db     = -120.0f;
    float     freq         = 0.0f;   // smoothed display frequency, 0 = none
    float     last_freq    = 0.0f;   // most recent non-zero freq, shown dimmed
    std::deque<HistPoint> history;

    // Reference tone / input
    std::vector<int> held;           // notes held by keys or mouse, newest last
    int  tone_note    = -1;
    int  mouse_note   = -1;

    // Window
    GLFWwindow* window       = nullptr;
    bool  settings_open      = false;
    bool  settings_dirty     = false;
    bool  fullscreen         = false;
    double fs_entered_t      = -100.0;
    bool  quit               = false;
    std::vector<std::string> devices;   // cached for the settings combo
    bool  restart_capture    = false;
};

// ui.cpp
void ui_load_fonts(float density);
void ui_apply_style();
void ui_frame(Tuner& t, double now);
void ui_set_tone(Tuner& t);   // push the held-note state to the tone generator
