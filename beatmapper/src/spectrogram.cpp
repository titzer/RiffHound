#include "spectrogram.h"
#include "imgui.h"
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#endif

// ---------------------------------------------------------------------------
// STFT parameters
// ---------------------------------------------------------------------------
static const int FFT_N    = 2048;   // FFT window size (must be power-of-two)
static const int HOP      = 512;    // hop size (75% overlap)
static const int BINS     = FFT_N / 2;   // 1024 bins, 0 Hz .. Nyquist
static const int MAX_TEXW = 8192;   // max texture width (time columns)

// ---------------------------------------------------------------------------
// Iterative Cooley-Tukey radix-2 FFT (in-place, decimation-in-time).
// re[] and im[] must each have n elements; n must be a power of two.
// ---------------------------------------------------------------------------
static void fft(float* re, float* im, int n)
{
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    // Butterfly stages
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * 3.14159265358979f / (float)len;
        float wr  = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int j = 0; j < (len >> 1); j++) {
                float ur = re[i+j],              ui = im[i+j];
                float vr = re[i+j+len/2]*cr - im[i+j+len/2]*ci;
                float vi = re[i+j+len/2]*ci + im[i+j+len/2]*cr;
                re[i+j]         = ur + vr;
                im[i+j]         = ui + vi;
                re[i+j+len/2]   = ur - vr;
                im[i+j+len/2]   = ui - vi;
                float nr = cr*wr - ci*wi;
                float ni = cr*wi + ci*wr;
                cr = nr; ci = ni;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Colormap: intensity [0,1] → RGB bytes.
// Inferno-like: near-black → dark-purple → orange → bright yellow-white.
// ---------------------------------------------------------------------------
static void colormap(float v, uint8_t* r, uint8_t* g, uint8_t* b)
{
    struct Stop { float t; int r, g, b; };
    static const Stop stops[] = {
        { 0.00f,   4,   2,  10 },
        { 0.20f,  30,  12,  80 },
        { 0.40f, 130,  25, 120 },
        { 0.60f, 220,  80,  20 },
        { 0.80f, 255, 200,  30 },
        { 1.00f, 255, 255, 220 },
    };
    static const int N = 6;
    if (v <= 0.0f) { *r = (uint8_t)stops[0].r; *g = (uint8_t)stops[0].g; *b = (uint8_t)stops[0].b; return; }
    if (v >= 1.0f) { *r = (uint8_t)stops[N-1].r; *g = (uint8_t)stops[N-1].g; *b = (uint8_t)stops[N-1].b; return; }
    for (int i = 0; i < N-1; i++) {
        if (v < stops[i+1].t) {
            float t = (v - stops[i].t) / (stops[i+1].t - stops[i].t);
            *r = (uint8_t)(stops[i].r + (int)(t * (stops[i+1].r - stops[i].r)));
            *g = (uint8_t)(stops[i].g + (int)(t * (stops[i+1].g - stops[i].g)));
            *b = (uint8_t)(stops[i].b + (int)(t * (stops[i+1].b - stops[i].b)));
            return;
        }
    }
    *r = (uint8_t)stops[N-1].r; *g = (uint8_t)stops[N-1].g; *b = (uint8_t)stops[N-1].b;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void spectrogram_init(SpectrogramState* s) {
    s->computed = false;
    s->duration = 0.0;
    s->texture  = 0;
    s->tex_w    = 0;
    s->tex_h    = 0;
}

void spectrogram_shutdown(SpectrogramState* s) {
    if (s->texture) {
        GLuint t = (GLuint)s->texture;
        glDeleteTextures(1, &t);
        s->texture = 0;
    }
    s->computed = false;
    s->duration = 0.0;
}

void spectrogram_compute(SpectrogramState* s,
                         const float* mono_samples,
                         uint64_t     num_samples,
                         uint32_t     sample_rate)
{
    if (!mono_samples || num_samples < (uint64_t)FFT_N || sample_rate == 0)
        return;

    int64_t num_frames = (int64_t)(num_samples - FFT_N) / HOP + 1;
    if (num_frames <= 0) return;

    int tex_w = (int)(num_frames < MAX_TEXW ? num_frames : MAX_TEXW);
    int tex_h = BINS;  // 1024

    // Hann window
    float window[FFT_N];
    for (int i = 0; i < FFT_N; i++)
        window[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265358979f * i / (FFT_N - 1)));

    uint8_t* pixels = (uint8_t*)malloc((size_t)tex_w * tex_h * 4);
    float*   re     = (float*)  malloc(FFT_N * sizeof(float));
    float*   im     = (float*)  malloc(FFT_N * sizeof(float));
    if (!pixels || !re || !im) {
        free(pixels); free(re); free(im);
        return;
    }

    float inv_norm = 1.0f / (FFT_N * 0.5f);  // normalize so 0 dBFS sine ≈ 1.0

    for (int col = 0; col < tex_w; col++) {
        // Map texture column linearly to an STFT frame
        int64_t frame  = (tex_w > 1) ? (int64_t)col * (num_frames - 1) / (tex_w - 1) : 0;
        int64_t offset = frame * HOP;

        for (int i = 0; i < FFT_N; i++) {
            int64_t idx = offset + i;
            float   samp = (idx < (int64_t)num_samples) ? mono_samples[idx] : 0.0f;
            re[i] = samp * window[i];
            im[i] = 0.0f;
        }

        fft(re, im, FFT_N);

        for (int bin = 0; bin < tex_h; bin++) {
            float mag = sqrtf(re[bin]*re[bin] + im[bin]*im[bin]) * inv_norm;
            float db  = 20.0f * log10f(mag + 1e-9f);

            // Map -80 dB .. 0 dB → [0, 1]
            float v = (db + 80.0f) / 80.0f;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;

            // bin 0 (DC / low freq) → bottom row; bin BINS-1 (Nyquist) → top row
            int row  = (tex_h - 1) - bin;
            int pidx = (row * tex_w + col) * 4;
            uint8_t r, g, b;
            colormap(v, &r, &g, &b);
            pixels[pidx + 0] = r;
            pixels[pidx + 1] = g;
            pixels[pidx + 2] = b;
            pixels[pidx + 3] = 255;
        }
    }

    free(re);
    free(im);

    // Delete old texture
    if (s->texture) {
        GLuint t = (GLuint)s->texture;
        glDeleteTextures(1, &t);
        s->texture = 0;
    }

    // Upload to GPU
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 tex_w, tex_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(pixels);

    s->texture  = (unsigned int)tex;
    s->tex_w    = tex_w;
    s->tex_h    = tex_h;
    s->duration = (double)num_samples / (double)sample_rate;
    s->computed = true;

    printf("[spectrogram] %d×%d  duration=%.1fs  frames=%lld\n",
           tex_w, tex_h, s->duration, (long long)num_frames);
}

void spectrogram_render(SpectrogramState* s, ImDrawList* dl,
                        float x, float y, float width, float height,
                        double view_start, double view_end)
{
    if (width <= 0.0f || height <= 0.0f) return;

    // Dark background always present
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height),
                      IM_COL32(18, 18, 30, 255));

    if (!s->computed || !s->texture || s->duration <= 0.0) {
        const char* msg = "No audio loaded";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(x + width  * 0.5f - ts.x * 0.5f,
                           y + height * 0.5f - ts.y * 0.5f),
                    IM_COL32(80, 80, 100, 255), msg);
        return;
    }

    // UV-X maps the view window onto [0, 1] across the full track duration
    float u0 = (float)(view_start / s->duration);
    float u1 = (float)(view_end   / s->duration);
    if (u0 < 0.0f) u0 = 0.0f;
    if (u1 > 1.0f) u1 = 1.0f;
    if (u1 <= u0) return;

    // UV-Y: row 0 in texture = high frequencies (Nyquist) = top of display
    dl->AddImage((ImTextureID)(intptr_t)s->texture,
                 ImVec2(x, y), ImVec2(x + width, y + height),
                 ImVec2(u0, 0.0f), ImVec2(u1, 1.0f));
}
