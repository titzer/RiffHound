#include "ui_timeline.h"
#include "imgui.h"
#include <math.h>
#include <stdio.h>

// --- helpers -----------------------------------------------------------

// Convert a time (seconds) to an x pixel within [origin_x, origin_x+width].
static float time_to_x(double t, double view_start, double view_end,
                        float origin_x, float width) {
    double span = view_end - view_start;
    if (span <= 0.0) return origin_x;
    return origin_x + (float)((t - view_start) / span * width);
}

// Pick a "nice" tick interval for the ruler given the visible time span.
// Returns major_interval (seconds) and sets *minor_div to the number of
// minor subdivisions within one major interval.
static double nice_interval(double span, int target_major_ticks, int* minor_div) {
    // Candidate major intervals: 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600...
    static const double candidates[] = {
        0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600
    };
    static const int minor_divs[] = {
        5,    5,    5,    5,  5, 4,  5,  5,  3,  6, 12,   4,   5,   6,    6,    4
    };
    int n = (int)(sizeof(candidates) / sizeof(candidates[0]));
    for (int i = 0; i < n; i++) {
        if (span / candidates[i] <= target_major_ticks) {
            if (minor_div) *minor_div = minor_divs[i];
            return candidates[i];
        }
    }
    if (minor_div) *minor_div = 4;
    return 3600.0;
}

// Format seconds as mm:ss or mm:ss.s depending on zoom level.
static void format_time(char* buf, int bufsize, double t, double major_interval) {
    int mins = (int)(t / 60.0);
    double secs = t - mins * 60.0;
    if (major_interval < 1.0) {
        snprintf(buf, bufsize, "%d:%05.2f", mins, secs);
    } else {
        snprintf(buf, bufsize, "%d:%02d", mins, (int)secs);
    }
}

// --- ruler drawing ------------------------------------------------------

static void draw_ruler(ImDrawList* dl, float rx, float ry, float rw, float rh,
                       double view_start, double view_end)
{
    // Background
    dl->AddRectFilled(ImVec2(rx, ry), ImVec2(rx + rw, ry + rh),
                      IM_COL32(30, 30, 40, 255));

    double span = view_end - view_start;
    if (span <= 0.0) return;

    int minor_div = 5;
    double major = nice_interval(span, 10, &minor_div);
    double minor = major / minor_div;

    // First major tick at or before view_start
    double first_major = floor(view_start / major) * major;

    for (double t = first_major; t <= view_end + major; t += major) {
        float x = time_to_x(t, view_start, view_end, rx, rw);
        if (x < rx || x > rx + rw) continue;

        // Major tick
        dl->AddLine(ImVec2(x, ry + rh * 0.4f), ImVec2(x, ry + rh),
                    IM_COL32(180, 180, 200, 255), 1.0f);

        // Label
        char buf[32];
        format_time(buf, sizeof(buf), t, major);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        if (x + ts.x + 2 < rx + rw) {
            dl->AddText(ImVec2(x + 2, ry + 2),
                        IM_COL32(200, 200, 220, 255), buf);
        }
    }

    // Minor ticks
    double first_minor = floor(view_start / minor) * minor;
    for (double t = first_minor; t <= view_end + minor; t += minor) {
        // Skip positions that coincide with a major tick
        if (fmod(t + 1e-9, major) < minor * 0.01) continue;
        float x = time_to_x(t, view_start, view_end, rx, rw);
        if (x < rx || x > rx + rw) continue;
        dl->AddLine(ImVec2(x, ry + rh * 0.7f), ImVec2(x, ry + rh),
                    IM_COL32(100, 100, 120, 200), 1.0f);
    }

    // Border
    dl->AddRect(ImVec2(rx, ry), ImVec2(rx + rw, ry + rh),
                IM_COL32(60, 60, 80, 255));
}

// --- playhead -----------------------------------------------------------

static void draw_playhead(ImDrawList* dl, float tx, float ty, float tw, float th,
                          double position, double view_start, double view_end)
{
    float x = time_to_x(position, view_start, view_end, tx, tw);
    if (x < tx || x > tx + tw) return;
    dl->AddLine(ImVec2(x, ty), ImVec2(x, ty + th),
                IM_COL32(255, 220, 50, 220), 1.5f);
    // Small triangle at top
    dl->AddTriangleFilled(
        ImVec2(x - 5, ty),
        ImVec2(x + 5, ty),
        ImVec2(x,     ty + 10),
        IM_COL32(255, 220, 50, 220));
}

// --- main widget --------------------------------------------------------

static const float RULER_H      = 24.0f;
static const float SPECTRO_H    = 200.0f;

void ui_timeline_render(EditorState* editor, AudioState* audio,
                        SpectrogramState* spectro)
{
    ImGuiIO& io = ImGui::GetIO();

    // Full-width child window
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float total_h = RULER_H + SPECTRO_H + 4.0f;
    if (avail.y < total_h) total_h = avail.y;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool visible = ImGui::BeginChild("##timeline", ImVec2(avail.x, total_h),
                                     false,
                                     ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    if (!visible) { ImGui::EndChild(); return; }

    ImVec2 canvas_pos  = ImGui::GetCursorScreenPos();
    float  canvas_w    = ImGui::GetContentRegionAvail().x;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    float rx = canvas_pos.x;
    float ry = canvas_pos.y;
    float rw = canvas_w;

    float tx = rx;
    float ty = ry + RULER_H + 2.0f;
    float tw = rw;
    float th = SPECTRO_H;

    // --- Input: zoom (Ctrl + mouse wheel) ---
    // Use an invisible button to capture input over the whole area
    ImGui::SetCursorScreenPos(ImVec2(rx, ry));
    ImGui::InvisibleButton("##timeline_input", ImVec2(rw, total_h),
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();

    if (hovered) {
        float mouse_x    = io.MousePos.x;
        float pixel_frac = (rw > 0) ? (mouse_x - rx) / rw : 0.5f;
        if (pixel_frac < 0) pixel_frac = 0;
        if (pixel_frac > 1) pixel_frac = 1;

        // Ctrl + scroll wheel → zoom
        if (io.KeyCtrl && io.MouseWheel != 0.0f) {
            editor_zoom(editor, pixel_frac, io.MouseWheel);
        }

        // Scroll wheel without Ctrl → pan
        if (!io.KeyCtrl && io.MouseWheel != 0.0f) {
            double span = editor->view_end - editor->view_start;
            editor_pan(editor, -io.MouseWheel * span * 0.1);
        }

        // Left drag → pan
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            float dx = io.MouseDelta.x;
            double span = editor->view_end - editor->view_start;
            if (rw > 0)
                editor_pan(editor, -dx / rw * span);
        }

        // Click in spectrogram area (not ruler) → seek
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            float my = io.MousePos.y;
            if (my > ty && my < ty + th) {
                double t = editor->view_start +
                           (mouse_x - tx) / tw *
                           (editor->view_end - editor->view_start);
                if (audio->loaded) audio_seek(audio, t);
            }
        }
    }

    // --- Draw ruler ---
    draw_ruler(dl, rx, ry, rw, RULER_H,
               editor->view_start, editor->view_end);

    // --- Draw spectrogram stub ---
    spectrogram_render(spectro, dl, tx, ty, tw, th,
                       editor->view_start, editor->view_end);

    // --- Draw playhead ---
    if (audio->loaded) {
        draw_playhead(dl, tx, ty, tw, th,
                      audio_get_position(audio),
                      editor->view_start, editor->view_end);
    }

    // --- Vertical line at cursor time (hover hint) ---
    if (hovered) {
        float mx = io.MousePos.x;
        if (mx >= rx && mx <= rx + rw) {
            dl->AddLine(ImVec2(mx, ry), ImVec2(mx, ty + th),
                        IM_COL32(255, 255, 255, 40), 1.0f);
            // Time tooltip
            double hover_t = editor->view_start +
                             (mx - rx) / rw *
                             (editor->view_end - editor->view_start);
            int hm = (int)(hover_t / 60.0);
            double hs = hover_t - hm * 60.0;
            char tip[32];
            snprintf(tip, sizeof(tip), "%d:%06.3f", hm, hs);
            dl->AddText(ImVec2(mx + 4, ry + 4),
                        IM_COL32(255, 255, 255, 180), tip);
        }
    }

    ImGui::EndChild();
}
