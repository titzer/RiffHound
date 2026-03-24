#include "ui_timeline.h"
#include "imgui.h"
#include <math.h>
#include <stdio.h>

// --- helpers -----------------------------------------------------------

static float time_to_x(double t, double view_start, double view_end,
                        float origin_x, float width) {
    double span = view_end - view_start;
    if (span <= 0.0) return origin_x;
    return origin_x + (float)((t - view_start) / span * width);
}

static double nice_interval(double span, int target_major_ticks, int* minor_div) {
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

static void format_time(char* buf, int bufsize, double t, double major_interval) {
    int mins = (int)(t / 60.0);
    double secs = t - mins * 60.0;
    if (major_interval < 1.0)
        snprintf(buf, bufsize, "%d:%05.2f", mins, secs);
    else
        snprintf(buf, bufsize, "%d:%02d", mins, (int)secs);
}

// --- ruler drawing ------------------------------------------------------

static void draw_ruler(ImDrawList* dl, float rx, float ry, float rw, float rh,
                       double view_start, double view_end)
{
    dl->AddRectFilled(ImVec2(rx, ry), ImVec2(rx + rw, ry + rh),
                      IM_COL32(30, 30, 40, 255));

    double span = view_end - view_start;
    if (span <= 0.0) return;

    int minor_div = 5;
    double major = nice_interval(span, 10, &minor_div);
    double minor = major / minor_div;

    double first_major = floor(view_start / major) * major;
    for (double t = first_major; t <= view_end + major; t += major) {
        float x = time_to_x(t, view_start, view_end, rx, rw);
        if (x < rx || x > rx + rw) continue;
        dl->AddLine(ImVec2(x, ry + rh * 0.4f), ImVec2(x, ry + rh),
                    IM_COL32(180, 180, 200, 255), 1.0f);
        char buf[32];
        format_time(buf, sizeof(buf), t, major);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        if (x + ts.x + 2 < rx + rw)
            dl->AddText(ImVec2(x + 2, ry + 2), IM_COL32(200, 200, 220, 255), buf);
    }

    double first_minor = floor(view_start / minor) * minor;
    for (double t = first_minor; t <= view_end + minor; t += minor) {
        if (fmod(t + 1e-9, major) < minor * 0.01) continue;
        float x = time_to_x(t, view_start, view_end, rx, rw);
        if (x < rx || x > rx + rw) continue;
        dl->AddLine(ImVec2(x, ry + rh * 0.7f), ImVec2(x, ry + rh),
                    IM_COL32(100, 100, 120, 200), 1.0f);
    }

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
    dl->AddTriangleFilled(ImVec2(x - 5, ty), ImVec2(x + 5, ty), ImVec2(x, ty + 10),
                          IM_COL32(255, 220, 50, 220));
}

// --- minimap drawing ----------------------------------------------------

static void draw_minimap(ImDrawList* dl,
                         float mx, float my, float mw, float mh,
                         double duration,
                         double view_start, double view_end,
                         bool has_region, double region_start, double region_end,
                         bool has_playhead, double playhead_pos)
{
    if (mw <= 0.0f || duration <= 0.0) return;

    dl->AddRectFilled(ImVec2(mx, my), ImVec2(mx + mw, my + mh),
                      IM_COL32(18, 18, 28, 255));

    if (has_region) {
        float r1 = mx + (float)(region_start / duration) * mw;
        float r2 = mx + (float)(region_end   / duration) * mw;
        r1 = r1 < mx ? mx : r1;
        r2 = r2 > mx + mw ? mx + mw : r2;
        if (r2 > r1)
            dl->AddRectFilled(ImVec2(r1, my), ImVec2(r2, my + mh),
                              IM_COL32(80, 140, 200, 70));
    }

    float vx1 = mx + (float)(view_start / duration) * mw;
    float vx2 = mx + (float)(view_end   / duration) * mw;
    vx1 = vx1 < mx ? mx : vx1;
    vx2 = vx2 > mx + mw ? mx + mw : vx2;
    if (vx2 > vx1) {
        dl->AddRectFilled(ImVec2(vx1, my), ImVec2(vx2, my + mh),
                          IM_COL32(80, 100, 140, 90));
        dl->AddRect(ImVec2(vx1, my), ImVec2(vx2, my + mh),
                    IM_COL32(140, 170, 220, 200), 0.0f, 0, 1.0f);
    }

    if (has_playhead) {
        float px = mx + (float)(playhead_pos / duration) * mw;
        if (px >= mx && px <= mx + mw)
            dl->AddLine(ImVec2(px, my), ImVec2(px, my + mh),
                        IM_COL32(255, 220, 50, 230), 1.5f);
    }

    dl->AddRect(ImVec2(mx, my), ImVec2(mx + mw, my + mh),
                IM_COL32(55, 55, 75, 255));
}

// --- beat area ----------------------------------------------------------

static const float BEAT_AREA_H  = 80.0f;
static const float DIAMOND_R    = 7.0f;
static const int   N_STAGGER    = 5;
static const float STAGGER_Y[N_STAGGER] = { 0.50f, 0.28f, 0.72f, 0.10f, 0.90f };

struct BeatVis { int idx; float bx, cy; };

static void draw_diamond(ImDrawList* dl, float cx, float cy, float r,
                         ImU32 fill, ImU32 border)
{
    ImVec2 pts[4] = {
        { cx,     cy - r },
        { cx + r, cy     },
        { cx,     cy + r },
        { cx - r, cy     },
    };
    dl->AddConvexPolyFilled(pts, 4, fill);
    dl->AddPolyline(pts, 4, border, ImDrawFlags_Closed, 1.5f);
}

// --- layout constants --------------------------------------------------

static const float RULER_H   = 24.0f;
static const float SPECTRO_H = 200.0f;
static const float MINIMAP_H = 40.0f;

// --- main widget -------------------------------------------------------

void ui_timeline_render(EditorState* editor, AudioState* audio,
                        SpectrogramState* spectro, BeatMap* beatmap)
{
    ImGuiIO& io = ImGui::GetIO();

    // Layout (top to bottom): minimap | ruler | spectrogram | beat area
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float total_h = MINIMAP_H + 2.0f + RULER_H + 2.0f + SPECTRO_H + 2.0f + BEAT_AREA_H;
    if (avail.y < total_h) total_h = avail.y;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool visible = ImGui::BeginChild("##timeline", ImVec2(avail.x, total_h), false,
                                     ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!visible) { ImGui::EndChild(); return; }

    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    float  canvas_w   = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl    = ImGui::GetWindowDrawList();

    float rx = canvas_pos.x, ry = canvas_pos.y, rw = canvas_w;

    float mm_x = rx, mm_y = ry,                       mm_w = rw, mm_h = MINIMAP_H;
    float ruler_y = mm_y + mm_h + 2.0f;
    float tx = rx,   ty = ruler_y + RULER_H + 2.0f,   tw = rw,   th = SPECTRO_H;
    float ba_x = rx, ba_y = ty + th + 2.0f,            ba_w = rw, ba_h = BEAT_AREA_H;

    // --- Beat position layout pass (rebuilds every frame) ---
    // Computes screen positions + stagger rows for all beats.
    // Used for both hit-testing (this frame) and drawing.
    static BeatVis s_vis[4096];
    static int     s_vis_n = 0;
    static int     s_sel_beat  = -1;  // selected beat index in beatmap->times[]
    static int     s_drag_beat = -1;  // beat being dragged; -1 = none
    static double  s_drag_new_t = 0.0;
    static double  s_drag_dt    = 0.0; // (beat_t - cursor_t) at drag start

    {
        s_vis_n = 0;
        float row_right[N_STAGGER];
        for (int r = 0; r < N_STAGGER; r++) row_right[r] = ba_x - 99999.0f;
        const float gap  = DIAMOND_R * 2.0f + 2.0f;
        double      span = editor->view_end - editor->view_start;

        for (int i = 0; i < beatmap->count; i++) {
            // Dragged beat uses its virtual position for display
            double t  = (s_drag_beat == i) ? s_drag_new_t : beatmap->times[i];
            float  bx = (span > 0.0)
                ? ba_x + (float)((t - editor->view_start) / span * ba_w)
                : ba_x;

            // Greedy first-fit row assignment (ensures consistent stagger across frames)
            int row = 0;
            for (int r = 0; r < N_STAGGER; r++) {
                if (bx - row_right[r] >= gap) { row = r; break; }
            }
            row_right[row] = bx;

            // Only emit visible beats into s_vis[]
            if (bx >= ba_x - DIAMOND_R && bx <= ba_x + ba_w + DIAMOND_R && s_vis_n < 4096)
                s_vis[s_vis_n++] = { i, bx, ba_y + STAGGER_Y[row] * ba_h };
        }
    }

    // --- Single InvisibleButton covering the whole timeline ---
    ImGui::SetCursorScreenPos(ImVec2(rx, ry));
    ImGui::InvisibleButton("##timeline_input", ImVec2(rw, total_h),
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();

    static bool   s_drag_in_spectro = false;
    static bool   s_drag_in_ruler   = false;
    static bool   s_mm_seeking      = false;
    static bool   s_drag_in_beats   = false;
    static double s_anchor          = 0.0;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float click_y = io.MousePos.y;
        s_drag_in_spectro = (click_y >= ty      && click_y < ty + th);
        s_drag_in_ruler   = (click_y >= ruler_y && click_y < ty);
        s_mm_seeking      = (click_y >= mm_y    && click_y < ruler_y);
        s_drag_in_beats   = (click_y >= ba_y    && click_y < ba_y + ba_h);

        if (s_drag_in_spectro) {
            double span = editor->view_end - editor->view_start;
            s_anchor = (tw > 0 && span > 0)
                ? editor->view_start + (io.MousePos.x - tx) / tw * span
                : editor->view_start;
            if (s_anchor < 0.0)              s_anchor = 0.0;
            if (s_anchor > editor->duration) s_anchor = editor->duration;
            editor->has_region = false;
        }

        if (s_drag_in_beats) {
            double span = editor->view_end - editor->view_start;
            double t_click = (ba_w > 0 && span > 0)
                ? editor->view_start + (io.MousePos.x - ba_x) / ba_w * span
                : 0.0;
            if (t_click < 0.0)              t_click = 0.0;
            if (t_click > editor->duration) t_click = editor->duration;

            // Hit-test visible beats using Manhattan distance (exact for diamonds)
            int   hit  = -1;
            float best = DIAMOND_R + 2.0f;
            for (int i = 0; i < s_vis_n; i++) {
                float dx = fabsf(io.MousePos.x - s_vis[i].bx);
                float dy = fabsf(io.MousePos.y - s_vis[i].cy);
                if (dx + dy <= best) { best = dx + dy; hit = i; }
            }

            if (editor->tool_mode == ToolMode::Place) {
                if (hit >= 0) {
                    // Click on existing beat → remove it
                    int idx = s_vis[hit].idx;
                    if      (s_sel_beat == idx)  s_sel_beat = -1;
                    else if (s_sel_beat >  idx)  s_sel_beat--;
                    beatmap_remove(beatmap, idx);
                } else {
                    // Click on empty space → add beat
                    int new_idx = beatmap_add(beatmap, t_click);
                    s_sel_beat = new_idx;
                }
                s_drag_beat = -1;
            } else {  // Select (and RegionSelect – treat same as Select in beat area)
                if (hit >= 0) {
                    s_sel_beat   = s_vis[hit].idx;
                    s_drag_beat  = s_sel_beat;
                    s_drag_new_t = beatmap->times[s_drag_beat];
                    s_drag_dt    = s_drag_new_t - t_click;
                } else {
                    s_sel_beat  = -1;
                    s_drag_beat = -1;
                }
            }
        }
    }

    // Beat drag: update virtual position while mouse is held
    if (s_drag_beat >= 0 && s_drag_in_beats &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        double span = editor->view_end - editor->view_start;
        double t_cursor = (ba_w > 0 && span > 0)
            ? editor->view_start + (io.MousePos.x - ba_x) / ba_w * span
            : 0.0;
        double new_t = t_cursor + s_drag_dt;
        if (new_t < 0.0)              new_t = 0.0;
        if (new_t > editor->duration) new_t = editor->duration;
        s_drag_new_t = new_t;
    }

    // Beat drag release: remove from old position, re-insert sorted at new position
    if (s_drag_beat >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        beatmap_remove(beatmap, s_drag_beat);
        int new_idx = beatmap_add(beatmap, s_drag_new_t);
        s_sel_beat  = (new_idx >= 0) ? new_idx : -1;
        s_drag_beat = -1;
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s_drag_in_spectro = false;
        s_drag_in_ruler   = false;
        s_mm_seeking      = false;
        s_drag_in_beats   = false;
    }

    // Minimap seek/scrub
    if (s_mm_seeking && mm_w > 0.0f && editor->duration > 0.0) {
        double t = (double)(io.MousePos.x - mm_x) / mm_w * editor->duration;
        if (t < 0.0)              t = 0.0;
        if (t > editor->duration) t = editor->duration;
        audio_seek(audio, t);
    }

    // Region drag in spectrogram
    if (s_drag_in_spectro && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        double span = editor->view_end - editor->view_start;
        double t_now = (tw > 0 && span > 0)
            ? editor->view_start + (io.MousePos.x - tx) / tw * span
            : editor->view_start;
        if (t_now < 0.0)              t_now = 0.0;
        if (t_now > editor->duration) t_now = editor->duration;
        editor->region_start = (s_anchor < t_now) ? s_anchor : t_now;
        editor->region_end   = (s_anchor < t_now) ? t_now    : s_anchor;
        editor->has_region   = (editor->region_end - editor->region_start) > 1e-6;
    }

    if (hovered) {
        float mouse_x    = io.MousePos.x;
        float pixel_frac = (rw > 0) ? (mouse_x - rx) / rw : 0.5f;
        if (pixel_frac < 0) pixel_frac = 0;
        if (pixel_frac > 1) pixel_frac = 1;

        if (io.KeyCtrl && io.MouseWheel != 0.0f)
            editor_zoom(editor, pixel_frac, io.MouseWheel);

        if (io.MouseWheelH != 0.0f) {
            double span = editor->view_end - editor->view_start;
            editor_pan(editor, io.MouseWheelH * span * 0.02);
        }

        if (s_drag_in_ruler && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            float dx = io.MouseDelta.x;
            double span = editor->view_end - editor->view_start;
            if (rw > 0) editor_pan(editor, -dx / rw * span);
        }
    }

    // --- Draw ---

    draw_minimap(dl, mm_x, mm_y, mm_w, mm_h,
                 editor->duration,
                 editor->view_start, editor->view_end,
                 editor->has_region, editor->region_start, editor->region_end,
                 audio->loaded, audio_get_position(audio));

    draw_ruler(dl, rx, ruler_y, rw, RULER_H,
               editor->view_start, editor->view_end);

    spectrogram_render(spectro, dl, tx, ty, tw, th,
                       editor->view_start, editor->view_end);

    // Region selection highlight
    if (editor->has_region) {
        float rx1 = time_to_x(editor->region_start, editor->view_start, editor->view_end, tx, tw);
        float rx2 = time_to_x(editor->region_end,   editor->view_start, editor->view_end, tx, tw);
        float cx1 = rx1 < tx      ? tx      : rx1;
        float cx2 = rx2 > tx + tw ? tx + tw : rx2;
        if (cx2 > cx1)
            dl->AddRectFilled(ImVec2(cx1, ty), ImVec2(cx2, ty + th),
                              IM_COL32(100, 180, 255, 55));
        if (rx1 >= tx && rx1 <= tx + tw)
            dl->AddLine(ImVec2(rx1, ty), ImVec2(rx1, ty + th),
                        IM_COL32(130, 200, 255, 230), 1.5f);
        if (rx2 >= tx && rx2 <= tx + tw)
            dl->AddLine(ImVec2(rx2, ty), ImVec2(rx2, ty + th),
                        IM_COL32(130, 200, 255, 230), 1.5f);
    }

    if (audio->loaded)
        draw_playhead(dl, tx, ty, tw, th,
                      audio_get_position(audio),
                      editor->view_start, editor->view_end);

    // Cursor time hint line + tooltip
    if (hovered) {
        float mx = io.MousePos.x;
        if (mx >= rx && mx <= rx + rw) {
            dl->AddLine(ImVec2(mx, ruler_y), ImVec2(mx, ty + th),
                        IM_COL32(255, 255, 255, 40), 1.0f);
            double hover_t = editor->view_start +
                             (mx - rx) / rw * (editor->view_end - editor->view_start);
            int hm = (int)(hover_t / 60.0);
            double hs = hover_t - hm * 60.0;
            char tip[32];
            snprintf(tip, sizeof(tip), "%d:%06.3f", hm, hs);
            dl->AddText(ImVec2(mx + 4, ruler_y + 4),
                        IM_COL32(255, 255, 255, 180), tip);
        }
    }

    // Beat area background
    dl->AddRectFilled(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h),
                      IM_COL32(14, 14, 22, 255));
    dl->AddRect(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h),
                IM_COL32(50, 50, 70, 255));

    // Diamonds (clipped to beat area)
    dl->PushClipRect(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h), true);
    for (int i = 0; i < s_vis_n; i++) {
        bool dragging = (s_vis[i].idx == s_drag_beat);
        bool selected = (s_vis[i].idx == s_sel_beat);
        ImU32 fill, border;
        if (dragging) {
            fill   = IM_COL32( 80, 220, 100, 220);
            border = IM_COL32(140, 255, 160, 255);
        } else if (selected) {
            fill   = IM_COL32(100, 160, 255, 220);
            border = IM_COL32(180, 210, 255, 255);
        } else {
            fill   = IM_COL32(255, 190,  40, 210);
            border = IM_COL32(255, 230, 100, 255);
        }
        draw_diamond(dl, s_vis[i].bx, s_vis[i].cy, DIAMOND_R, fill, border);
    }
    dl->PopClipRect();

    ImGui::EndChild();
}
