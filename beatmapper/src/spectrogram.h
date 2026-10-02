#pragma once

#include <stdint.h>

// Spectrogram module: STFT via hand-rolled Cooley-Tukey FFT, GPU texture, render.

struct SpectrogramState {
    bool         computed;
    double       duration;    // seconds; set by spectrogram_compute
    unsigned int texture;     // GLuint (stored as uint to avoid GL headers here)
    unsigned int intensity;   // GLuint, single-channel [0,1] magnitude (3D alpha)
    int          tex_w;       // texture width  (time columns)
    int          tex_h;       // texture height (frequency bins)
    unsigned int sample_rate; // native sample rate (for frequency axis labels)
};

// CPU half of a spectrogram: the colour-mapped image and the raw intensity,
// ready to upload.  Built on any thread; uploaded on the GL thread.
struct SpectrogramPixels {
    uint8_t* rgba;        // tex_w * tex_h * 4, row 0 = Nyquist
    uint8_t* intensity;   // tex_w * tex_h, same layout, 0..255 = -80..0 dB
    int      tex_w, tex_h;
    double   duration;
    uint32_t sample_rate;
};

void spectrogram_init(SpectrogramState* s);
void spectrogram_shutdown(SpectrogramState* s);

// Compute STFT from mono f32 PCM and upload to a GPU texture.
// Must be called from the GL thread (i.e. the main thread).
void spectrogram_compute(SpectrogramState* s,
                         const float* mono_samples,
                         uint64_t     num_samples,
                         uint32_t     sample_rate);

// The two halves of spectrogram_compute.  compute_pixels touches no GL state
// and may run on a worker thread; upload takes the GL thread, replaces the
// textures in `s` and frees the pixel buffers.
bool spectrogram_compute_pixels(SpectrogramPixels* px,
                                const float* mono_samples,
                                uint64_t     num_samples,
                                uint32_t     sample_rate);
void spectrogram_upload(SpectrogramState* s, SpectrogramPixels* px);
void spectrogram_pixels_free(SpectrogramPixels* px);

// Minimum frequency (Hz) for the logarithmic axis display.
static constexpr float SPECTRO_LOG_FMIN = 20.0f;

// Render into the current ImGui window's draw list.
// Draws in the rect [x, y, x+width, y+height].
// view_start/view_end are the visible time range in seconds.
// max_freq: highest frequency (Hz) to display; clamped to [0, Nyquist].
// log_freq: if true, map the y axis logarithmically (SPECTRO_LOG_FMIN..max_freq).
struct ImDrawList;
void spectrogram_render(SpectrogramState* s, ImDrawList* dl,
                        float x, float y, float width, float height,
                        double view_start, double view_end, float max_freq,
                        bool log_freq = false);
