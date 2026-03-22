#include "spectrogram.h"
#include "imgui.h"
#include <math.h>

void spectrogram_init(SpectrogramState* s) {
    s->computed = false;
    s->duration = 0.0;
}

void spectrogram_set_duration(SpectrogramState* s, double duration) {
    s->duration = duration;
}

void spectrogram_shutdown(SpectrogramState* s) {
    s->computed = false;
}

// Phase 0 stub: draw a placeholder gradient to fill the spectrogram area.
// Columns vary in color by time position so scrolling/zoom is visually obvious.
void spectrogram_render(SpectrogramState* s, ImDrawList* dl,
                        float x, float y, float width, float height,
                        double view_start, double view_end)
{
    if (width <= 0 || height <= 0) return;

    // Dark background
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height),
                      IM_COL32(18, 18, 30, 255));

    if (!s->computed || s->duration <= 0.0) {
        // Show "No audio loaded" message
        const char* msg = "No audio loaded";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(x + width * 0.5f - ts.x * 0.5f,
                           y + height * 0.5f - ts.y * 0.5f),
                    IM_COL32(80, 80, 100, 255), msg);
        return;
    }

    // Stub gradient: vertical bands whose color cycles with time position.
    // This makes zoom and pan visually verifiable without real FFT data.
    int num_bands = (int)width;
    for (int i = 0; i < num_bands; i++) {
        double t = view_start + (view_end - view_start) * (i / (double)width);
        // Slow color cycle based on time position
        float hue = (float)fmod(t * 0.05, 1.0);
        // Convert hue to rough RGB
        float r = 0.5f + 0.4f * (float)sin(hue * 6.283185f + 0.0f);
        float g = 0.5f + 0.4f * (float)sin(hue * 6.283185f + 2.094f);
        float b = 0.5f + 0.4f * (float)sin(hue * 6.283185f + 4.189f);
        // Darken towards bottom (fake frequency dimension)
        ImU32 col_top = IM_COL32((int)(r * 80), (int)(g * 80), (int)(b * 180), 255);
        ImU32 col_bot = IM_COL32((int)(r * 20), (int)(g * 20), (int)(b * 40), 255);
        dl->AddRectFilledMultiColor(
            ImVec2(x + i, y), ImVec2(x + i + 1.0f, y + height),
            col_top, col_top, col_bot, col_bot);
    }
}
