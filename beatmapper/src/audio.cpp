#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio.h"
#include <string.h>
#include <stdio.h>

static ma_engine s_engine;
static ma_sound  s_sound;
static bool      s_engine_ok = false;
static bool      s_sound_ok  = false;

void audio_init(AudioState* a) {
    memset(a, 0, sizeof(*a));
    if (ma_engine_init(NULL, &s_engine) != MA_SUCCESS) {
        fprintf(stderr, "[audio] failed to init miniaudio engine\n");
        return;
    }
    s_engine_ok = true;
}

bool audio_load(AudioState* a, const char* path) {
    if (!s_engine_ok) return false;

    // Tear down the previous sound if one was loaded.
    if (s_sound_ok) {
        ma_sound_uninit(&s_sound);
        s_sound_ok = false;
    }
    a->loaded   = false;
    a->playing  = false;
    a->position = 0.0;

    ma_result result = ma_sound_init_from_file(
        &s_engine, path,
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
        NULL, NULL, &s_sound);

    if (result != MA_SUCCESS) {
        fprintf(stderr, "[audio] failed to load '%s' (miniaudio error %d)\n", path, result);
        return false;
    }
    s_sound_ok = true;

    float length_sec = 0.0f;
    ma_sound_get_length_in_seconds(&s_sound, &length_sec);

    strncpy(a->filename, path, sizeof(a->filename) - 1);
    a->filename[sizeof(a->filename) - 1] = '\0';
    a->duration = (double)length_sec;
    a->position = 0.0;
    a->playing  = false;
    a->loaded   = true;

    printf("[audio] loaded '%s'  duration=%.2fs\n", path, a->duration);
    return true;
}

void audio_play(AudioState* a) {
    if (!s_sound_ok || !a->loaded) return;
    ma_sound_start(&s_sound);
    a->playing = true;
}

void audio_pause(AudioState* a) {
    if (!s_sound_ok) return;
    ma_sound_stop(&s_sound);
    a->playing = false;
}

void audio_seek(AudioState* a, double time_sec) {
    if (!s_sound_ok || !a->loaded) return;
    if (time_sec < 0.0)          time_sec = 0.0;
    if (time_sec > a->duration)  time_sec = a->duration;

    ma_uint32 sample_rate = ma_engine_get_sample_rate(&s_engine);
    ma_uint64 frame       = (ma_uint64)(time_sec * sample_rate + 0.5);
    ma_sound_seek_to_pcm_frame(&s_sound, frame);
    a->position = time_sec;
}

double audio_get_position(AudioState* a) {
    return a->position;
}

void audio_update(AudioState* a) {
    if (!s_sound_ok || !a->loaded) return;

    // Sync playing flag from the audio thread.
    a->playing = (bool)ma_sound_is_playing(&s_sound);

    // Sync cursor position.
    float cursor_sec = 0.0f;
    if (ma_sound_get_cursor_in_seconds(&s_sound, &cursor_sec) == MA_SUCCESS)
        a->position = (double)cursor_sec;
}

void audio_shutdown(AudioState* a) {
    if (s_sound_ok) {
        ma_sound_uninit(&s_sound);
        s_sound_ok = false;
    }
    if (s_engine_ok) {
        ma_engine_uninit(&s_engine);
        s_engine_ok = false;
    }
    a->loaded  = false;
    a->playing = false;
}
