#pragma once

#include <stdint.h>

// Spectrogram module: STFT via hand-rolled Cooley-Tukey FFT, GPU texture, render.

struct SpectrogramState {
    bool         computed;
    double       duration;    // seconds; set by spectrogram_compute
    unsigned int texture;     // GLuint (stored as uint to avoid GL headers here)
    int          tex_w;       // texture width  (time columns)
    int          tex_h;       // texture height (frequency bins)
};

void spectrogram_init(SpectrogramState* s);
void spectrogram_shutdown(SpectrogramState* s);

// Compute STFT from mono f32 PCM and upload to a GPU texture.
// Must be called from the GL thread (i.e. the main thread).
void spectrogram_compute(SpectrogramState* s,
                         const float* mono_samples,
                         uint64_t     num_samples,
                         uint32_t     sample_rate);

// Render into the current ImGui window's draw list.
// Draws in the rect [x, y, x+width, y+height].
// view_start/view_end are the visible time range in seconds.
struct ImDrawList;
void spectrogram_render(SpectrogramState* s, ImDrawList* dl,
                        float x, float y, float width, float height,
                        double view_start, double view_end);
