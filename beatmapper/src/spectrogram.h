#pragma once

// Spectrogram module: STFT via pffft, tiled GPU texture cache, render.
// Phase 0: stub – renders a placeholder colored gradient.
// Phase 1: compute STFT from PCM, upload as GPU textures.

struct SpectrogramState {
    bool computed;
    double duration;  // seconds; mirrors audio duration
};

void spectrogram_init(SpectrogramState* s);
void spectrogram_set_duration(SpectrogramState* s, double duration);
void spectrogram_shutdown(SpectrogramState* s);

// Render into the current ImGui window's draw list.
// Draws in the rect [x, y, x+width, y+height].
// view_start/view_end are the visible time range in seconds.
struct ImDrawList;
void spectrogram_render(SpectrogramState* s, ImDrawList* dl,
                        float x, float y, float width, float height,
                        double view_start, double view_end);
