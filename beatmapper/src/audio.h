#pragma once

#include <stdint.h>

// Audio module: miniaudio + WSOLA pitch-preserving time stretching

struct AudioState {
    bool   loaded;
    bool   playing;
    double duration;    // seconds; set on load
    double position;    // seconds; updated each frame by audio_update
    double play_start;  // position at which the last play was initiated
    float  speed;       // playback speed [0.25, 2.0], default 1.0
    char   filename[512];
};

void   audio_init(AudioState* a);
bool   audio_load(AudioState* a, const char* path);
void   audio_play(AudioState* a);
void   audio_pause(AudioState* a);
void   audio_seek(AudioState* a, double time_sec);
double audio_get_position(AudioState* a);

// Clamp speed to [0.25, 2.0] and round to nearest 0.05.
void   audio_set_speed(AudioState* a, float speed);
float  audio_get_speed(AudioState* a);

// Call once per frame to sync position/playing state from the audio thread.
void   audio_update(AudioState* a);

void   audio_shutdown(AudioState* a);

// Decode the file to mono f32 PCM for offline processing (e.g. spectrogram).
// Returns true on success; caller must free *out_samples with audio_free_pcm().
bool   audio_decode_pcm(const char* path, float** out_samples,
                        uint64_t* out_frame_count, uint32_t* out_sample_rate);
void   audio_free_pcm(float* samples);
