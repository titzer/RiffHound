#pragma once

// Audio module: miniaudio wrapper

struct AudioState {
    bool   loaded;
    bool   playing;
    double duration;   // seconds; set on load
    double position;   // seconds; updated each frame by audio_update
    char   filename[512];
};

void   audio_init(AudioState* a);
bool   audio_load(AudioState* a, const char* path);
void   audio_play(AudioState* a);
void   audio_pause(AudioState* a);
void   audio_seek(AudioState* a, double time_sec);
double audio_get_position(AudioState* a);

// Call once per frame to sync position/playing state from the audio thread.
void   audio_update(AudioState* a);

void   audio_shutdown(AudioState* a);
