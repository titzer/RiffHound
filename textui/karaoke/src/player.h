#pragma once
// Transport, loops and (stubbed) tuning, over beatmapper's audio engine
// (beatmapper/src/audio.cpp: miniaudio output, CoreAudio/miniaudio decoding,
// and the stretch engine that will do speed and pitch).

#include <string>
#include <vector>

#include "audio.h"
#include "chart.h"

struct LoopEntry {
    std::string name;
    double from, to;
};

struct Player {
    AudioState  audio;
    EditorState editor;     // audio.cpp keeps speed/pitch/engine here
    bool        ready = false;

    // The loop programme: chart loops queued in order and played round and
    // round, like the web client's loop buffer.
    std::vector<LoopEntry> programme;
    int  current = 0;       // the entry playing

    // Tuning and speed: kept and shown, not yet applied (see apply_tuning).
    float  speed = 1.0f;    // 0.50 .. 1.50
    int    semitones = 0;   // -12 .. 12
    int    cents = 0;       // -100 .. 100

    bool   init();
    bool   load(const std::string& path);
    void   shutdown();

    bool   playing() const { return audio.playing; }
    double position() const { return audio.position; }
    double duration() const { return audio.duration; }

    void   toggle();
    void   seek(double t);
    void   nudge(double dt);    // inside the current loop when one is playing
    void   update();            // once a frame

    void   add_loop(const Loop& l);
    void   remove_last_loop();
    void   clear_loops();
    bool   looping() const { return !programme.empty(); }

    void   set_speed(float s);
    void   transpose(int semis, int cents);
    void   reset_tuning();

private:
    double last_pos = 0;
    int    quiet = 0;       // frames to ignore backwards jumps, after a seek
    void   engage(int entry);
    void   apply_tuning();
};
