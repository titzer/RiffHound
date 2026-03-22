#include "audio.h"
#include <string.h>
#include <stdio.h>

// Phase 0 stub implementation.
// Phase 1 will replace this with full miniaudio integration.

void audio_init(AudioState* a) {
    memset(a, 0, sizeof(*a));
}

bool audio_load(AudioState* a, const char* path) {
    strncpy(a->filename, path, sizeof(a->filename) - 1);
    // Stub: pretend we loaded a 3-minute track.
    a->duration = 180.0;
    a->position = 0.0;
    a->playing  = false;
    a->loaded   = true;
    printf("[audio] stub load: %s (duration=%.1fs)\n", path, a->duration);
    return true;
}

void audio_play(AudioState* a) {
    if (a->loaded) a->playing = true;
}

void audio_pause(AudioState* a) {
    a->playing = false;
}

void audio_seek(AudioState* a, double time_sec) {
    if (!a->loaded) return;
    if (time_sec < 0.0) time_sec = 0.0;
    if (time_sec > a->duration) time_sec = a->duration;
    a->position = time_sec;
}

double audio_get_position(AudioState* a) {
    return a->position;
}

void audio_shutdown(AudioState* a) {
    a->loaded  = false;
    a->playing = false;
}
