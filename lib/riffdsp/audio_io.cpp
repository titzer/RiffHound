#include "audio_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t SAMPLE_RATE = 44100;
static const uint32_t CHANNELS    = 2;

// Quote a path for /bin/sh so spaces and apostrophes in track names survive.
// "The Rover.mp3" and "Don't Look Back.m4a" both occur in the library.
static void shell_quote(const char* in, char* out, size_t out_size) {
    size_t o = 0;
    if (o < out_size) out[o++] = '\'';
    for (const char* p = in; *p && o + 4 < out_size; p++) {
        if (*p == '\'') {
            // close quote, escaped quote, reopen
            const char* esc = "'\\''";
            for (int k = 0; k < 4 && o < out_size - 1; k++) out[o++] = esc[k];
        } else {
            out[o++] = *p;
        }
    }
    if (o < out_size) out[o++] = '\'';
    out[o < out_size ? o : out_size - 1] = '\0';
}

bool audio_decode(const char* path, float** out_pcm,
                  uint64_t* out_frames, uint32_t* out_channels,
                  uint32_t* out_sample_rate) {
    char quoted[1200];
    shell_quote(path, quoted, sizeof(quoted));

    char cmd[1400];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v error -i %s -f f32le -acodec pcm_f32le -ac %u -ar %u - 2>/dev/null",
             quoted, CHANNELS, SAMPLE_RATE);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        fprintf(stderr, "riffdsp: cannot run ffmpeg (is it installed?)\n");
        return false;
    }

    size_t cap    = 1u << 22;   // 4M floats ≈ 47 s of stereo
    size_t total  = 0;
    float* buf    = (float*)malloc(cap * sizeof(float));
    if (!buf) { pclose(pipe); return false; }

    while (true) {
        if (total == cap) {
            size_t ncap = cap * 2;
            float* nb = (float*)realloc(buf, ncap * sizeof(float));
            if (!nb) { free(buf); pclose(pipe); return false; }
            buf = nb;
            cap = ncap;
        }
        size_t got = fread(buf + total, sizeof(float), cap - total, pipe);
        if (got == 0) break;
        total += got;
    }
    int rc = pclose(pipe);
    if (total == 0) {
        free(buf);
        fprintf(stderr, "riffdsp: ffmpeg produced no audio for '%s' (exit %d)\n", path, rc);
        return false;
    }

    *out_pcm         = buf;
    *out_frames      = total / CHANNELS;
    *out_channels    = CHANNELS;
    *out_sample_rate = SAMPLE_RATE;
    return true;
}
