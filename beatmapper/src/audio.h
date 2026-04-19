#pragma once

#include <stdint.h>
#include "editor.h"

// Audio module: miniaudio + WSOLA pitch-preserving time stretching

struct AudioState {
    bool   loaded;
    bool   playing;
    bool   loop;        // loop mode on/off
    double duration;    // seconds; set on load
    double position;    // seconds; updated each frame by audio_update
    double play_start;  // position at which the last play was initiated
    char   filename[512];
};

void   audio_init(AudioState* a);
// Load an audio file. Resets e->speed to 1.0 and e->semitones/cents to 0 on success.
bool   audio_load(AudioState* a, EditorState* e, const char* path);
void   audio_play(AudioState* a);
void   audio_pause(AudioState* a);
void   audio_seek(AudioState* a, double time_sec);
double audio_get_position(AudioState* a);

// Clamp speed to [0.25, 2.0] and round to nearest 0.05; stores result in e->speed.
void   audio_set_speed(EditorState* e, float speed);

// Set pitch shift. semitones in [-12, 12], cents in [-100, 100].
// Pitch ratio = 2^((semitones*100 + cents) / 1200). Stores result in e->semitones/cents.
void   audio_set_pitch(EditorState* e, int semitones, int cents);

// Set loop mode and region. When enabled, playback wraps from loop_end back to
// loop_start seamlessly. Pass loop_start=0, loop_end=duration to loop the whole track.
void   audio_set_loop(AudioState* a, bool enabled, double loop_start, double loop_end);

// Call once per frame to sync position/playing state from the audio thread.
void   audio_update(AudioState* a);

void   audio_shutdown(AudioState* a);

// Decode the file to mono f32 PCM for offline processing (e.g. spectrogram).
// Returns true on success; caller must free *out_samples with audio_free_pcm().
bool   audio_decode_pcm(const char* path, float** out_samples,
                        uint64_t* out_frame_count, uint32_t* out_sample_rate);
void   audio_free_pcm(float* samples);
