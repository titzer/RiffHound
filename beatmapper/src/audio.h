#pragma once

// Audio module: miniaudio wrapper
// Phase 0: stub – loads file metadata, play/pause/seek not yet wired to hardware

struct AudioState {
    bool   loaded;
    bool   playing;
    double duration;   // seconds; set on load
    double position;   // seconds; stub advances in main loop
    char   filename[512];
};

void   audio_init(AudioState* a);
bool   audio_load(AudioState* a, const char* path);  // stub: sets duration placeholder
void   audio_play(AudioState* a);
void   audio_pause(AudioState* a);
void   audio_seek(AudioState* a, double time_sec);
double audio_get_position(AudioState* a);
void   audio_shutdown(AudioState* a);
