#include "ui_timeline.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "undo.h"
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

// Snap t to the nearest beat within snap_px pixels; returns t unchanged if no
// beat is within tolerance or beatmap is empty.
static double snap_to_beat(double t, const BeatMap* bm,
                            double view_start, double view_end, float area_w,
                            float snap_px = 12.0f)
{
    if (bm->count == 0 || area_w <= 0) return t;
    double span = view_end - view_start;
    double tol  = (double)(snap_px / area_w) * span;
    int lo = 0, hi = bm->count;
    while (lo < hi) { int m = (lo+hi)/2; if (bm->beats[m].time < t) lo=m+1; else hi=m; }
    double best = t, bd = tol + 1e-9;
    if (lo > 0) {
        double d = t - bm->beats[lo-1].time;
        if (d < bd) { bd = d; best = bm->beats[lo-1].time; }
    }
    if (lo < bm->count) {
        double d = bm->beats[lo].time - t;
        if (d < bd) { best = bm->beats[lo].time; }
    }
    return best;
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

static const float BEAT_AREA_H    = 80.0f;
static const float DIAMOND_R      = 7.0f;   // fixed (hand-placed / committed) beats
static const float DIAMOND_R_INTERP = 4.5f; // interpolated beats
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

static const float RULER_H       = 24.0f;
static const float MINIMAP_H     = 40.0f;
static const float CTX_PANEL_H   = 36.0f;  // contextual interpolate panel
static const float PLACE_STRIP_H = 22.0f;  // beat placement strip
static const float SECTION_H     = 52.0f;  // section strip
static const float LYRIC_H       = 36.0f;  // lyric strip
static const float EDIT_PANEL_H  = 34.0f;  // unified edit panel (section or lyric) at bottom

// Per-kind fill and border colours (index = SectionKind)
static const ImU32 s_sec_fill[SK_COUNT] = {
    IM_COL32( 70, 130, 210, 170),  // intro        - blue
    IM_COL32( 50, 160,  80, 170),  // verse        - green
    IM_COL32(155, 185,  50, 170),  // pre-chorus   - yellow-green
    IM_COL32(220, 120,  40, 170),  // chorus       - orange
    IM_COL32(200,  60,  60, 170),  // post-chorus  - red
    IM_COL32(130,  70, 200, 170),  // bridge       - purple
    IM_COL32( 90,  90, 110, 170),  // breakdown    - slate
    IM_COL32( 40, 170, 160, 170),  // instrumental - teal
    IM_COL32(200, 170,  30, 170),  // solo         - gold
    IM_COL32(180,  70, 170, 170),  // interlude    - violet
    IM_COL32( 50,  90, 170, 170),  // outro        - dark blue
    IM_COL32(190,  50, 100, 170),  // refrain      - crimson
};
static const ImU32 s_sec_border[SK_COUNT] = {
    IM_COL32(120, 180, 255, 230),  // intro
    IM_COL32(100, 210, 130, 230),  // verse
    IM_COL32(200, 230,  90, 230),  // pre-chorus
    IM_COL32(255, 170,  80, 230),  // chorus
    IM_COL32(240, 110, 110, 230),  // post-chorus
    IM_COL32(180, 120, 250, 230),  // bridge
    IM_COL32(140, 140, 165, 230),  // breakdown
    IM_COL32( 80, 220, 210, 230),  // instrumental
    IM_COL32(240, 210,  80, 230),  // solo
    IM_COL32(225, 120, 220, 230),  // interlude
    IM_COL32(100, 140, 220, 230),  // outro
    IM_COL32(230,  90, 145, 230),  // refrain
};

// --- main widget -------------------------------------------------------

void ui_timeline_render(EditorState* editor, AudioState* audio,
                        SpectrogramState* spectro, BeatMap* beatmap,
                        UndoStack* undo, SectionMap* sectionmap,
                        LyricMap* lyricmap)
{
    ImGuiIO& io = ImGui::GetIO();

    static bool s_beats_collapsed = false;   // beat editor collapsed to slim display strip
    static int  s_spectro_max_khz = 22;      // max displayed frequency [2, 22] kHz

    // Pre-compute contextual panel visibility (needs beatmap state, but before BeginChild
    // so we can set the correct child height).
    // Show when exactly 2 adjacent beats are selected AND the gap fits at least one
    // interpolated beat at the instantaneous BPM entering the first selected beat.
    int ctx_sel[2] = { -1, -1 };
    {
        int n = 0;
        for (int i = 0; i < beatmap->count && n < 3; i++)
            if (beatmap->beats[i].selected) { if (n < 2) ctx_sel[n] = i; n++; }
        if (n != 2 || ctx_sel[1] != ctx_sel[0] + 1) ctx_sel[0] = ctx_sel[1] = -1;
    }
    bool show_ctx = false;
    if (ctx_sel[0] >= 0) {
        double t1 = beatmap->beats[ctx_sel[0]].time;
        double t2 = beatmap->beats[ctx_sel[1]].time;
        double dt = t2 - t1;
        // BPM reference: instantaneous tempo entering the first selected beat
        double bpm_ref = 120.0;
        if (ctx_sel[0] > 0) {
            double d = t1 - beatmap->beats[ctx_sel[0] - 1].time;
            if (d > 1e-6) bpm_ref = 60.0 / d;
        }
        show_ctx = ((int)round(dt * bpm_ref / 60.0) >= 2);
    }
    float ctx_h = show_ctx ? CTX_PANEL_H : 0.0f;
    // Ctx panel and beat editing are hidden when the beat editor is collapsed.
    if (s_beats_collapsed) { show_ctx = false; ctx_h = 0.0f; }

    // Persistent selection indices for section and lyric strips.
    static int s_sec_selected = -1;
    if (s_sec_selected >= sectionmap->count) s_sec_selected = -1;
    sectionmap->selected_idx = s_sec_selected;  // keep struct in sync for main.cpp delete

    static int s_lyr_selected = -1;
    if (s_lyr_selected >= lyricmap->count) s_lyr_selected = -1;
    lyricmap->selected_idx = s_lyr_selected;  // keep struct in sync for main.cpp delete

    // Layout (top to bottom):
    //   minimap | ruler | spectrogram (flexible) |
    //   place strip | beat area | [ctx panel] | section strip | lyric strip | edit panel
    // The edit panel is always reserved at a fixed height at the very bottom and shows
    // section or lyric widgets depending on which is currently selected.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float fixed_h = MINIMAP_H + 2.0f + RULER_H + 2.0f
                  + 2.0f + PLACE_STRIP_H + 2.0f
                  + (s_beats_collapsed ? 0.0f : BEAT_AREA_H + ctx_h)
                  + 2.0f + SECTION_H
                  + 2.0f + LYRIC_H + EDIT_PANEL_H;
    float spectro_h = avail.y - fixed_h;
    if (spectro_h < 50.0f) spectro_h = 50.0f;
    float total_h = fixed_h + spectro_h;

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

    // Left sidebar: just wide enough to show "20k" with 4 px padding on each side.
    float sidebar_w = ImGui::CalcTextSize("20k").x + 8.0f;
    float cx = rx + sidebar_w;   // content area left edge
    float cw = rw - sidebar_w;   // content area width

    float mm_x = cx, mm_y = ry,                       mm_w = cw, mm_h = MINIMAP_H;
    float ruler_y = mm_y + mm_h + 2.0f;
    float tx = cx,   ty = ruler_y + RULER_H + 2.0f,   tw = cw,   th = spectro_h;
    float ps_x = cx, ps_y = ty + th + 2.0f,            ps_w = cw; // placement strip
    float ba_x = cx, ba_y = ps_y + PLACE_STRIP_H + 2.0f, ba_w = cw,
          ba_h = s_beats_collapsed ? 0.0f : BEAT_AREA_H;
    // ctx panel sits between beat area and section strip (ctx_y defined after ba_y)
    float ctx_y  = ba_y + ba_h;
    float sa_x   = cx,  sa_y = ctx_y + ctx_h + 2.0f, sa_w = cw, sa_h = SECTION_H;
    float la_x   = cx,  la_y = sa_y + SECTION_H + 2.0f, la_w = cw, la_h = LYRIC_H;
    float ep_y   = la_y + la_h;   // unified edit panel top (always reserved)

    // --- Beat position layout pass (rebuilds every frame) ---
    // Computes screen positions + stagger rows for all beats.
    // Used for both hit-testing (this frame) and drawing.
    static BeatVis s_vis[4096];
    static int     s_vis_n = 0;
    static int     s_drag_beat  = -1;  // beat being dragged; -1 = none
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
            double t  = (s_drag_beat == i) ? s_drag_new_t : beatmap->beats[i].time;
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

    // --- Single InvisibleButton covering the timeline (not the ctx panel) ---
    // AllowOverlap so that sidebar widgets (frequency +/- buttons, collapse triangle)
    // added later in the frame can still receive hover and clicks.
    ImGui::SetNextItemAllowOverlap();
    ImGui::SetCursorScreenPos(ImVec2(rx, ry));
    ImGui::InvisibleButton("##timeline_input",
                           ImVec2(rw, total_h - ctx_h - EDIT_PANEL_H),
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();

    static bool   s_drag_in_spectro = false;
    static bool   s_drag_in_ruler   = false;
    static bool   s_mm_seeking      = false;
    static bool   s_drag_in_place   = false;
    static bool   s_drag_in_beats   = false;
    static bool   s_drag_in_sec     = false;   // drag started in section strip
    static bool   s_drag_in_lyr     = false;   // drag started in lyric strip
    // Section editing state
    static bool   s_sec_drag        = false;   // drag-to-create in progress
    static double s_sec_drag_t0     = 0.0;
    static double s_sec_drag_t1     = 0.0;
    static bool   s_sec_hdrag       = false;   // handle resize drag in progress
    static int    s_sec_hdrag_idx   = -1;      // which section is handle-dragged
    static int    s_sec_hdrag_end   = 0;       // 0 = start handle, 1 = end handle
    // Lyric editing state
    static bool   s_lyr_drag        = false;   // drag-to-create in progress
    static double s_lyr_drag_t0     = 0.0;
    static double s_lyr_drag_t1     = 0.0;
    static bool   s_lyr_hdrag       = false;   // handle resize drag in progress
    static int    s_lyr_hdrag_idx   = -1;
    static int    s_lyr_hdrag_end   = 0;
    static bool   s_rect_sel        = false;  // rect selection in progress
    static float  s_rect_x0         = 0.0f, s_rect_y0 = 0.0f;
    static double s_anchor          = 0.0;

    // Contextual interpolation panel state
    static float s_ctx_bpm   = 120.0f;
    static int   s_ctx_count = 1;
    static int   s_ctx_prev0 = -1, s_ctx_prev1 = -1;  // last seen pair to detect changes
    static bool  s_ctx_hover = false;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float click_x = io.MousePos.x;
        float click_y = io.MousePos.y;

        // Triangle in left sidebar: toggle beat editor collapse (eats the click)
        if (click_x < cx && click_y >= ps_y && click_y < ps_y + PLACE_STRIP_H)
            s_beats_collapsed = !s_beats_collapsed;

        s_drag_in_spectro = (click_x >= cx && click_y >= ty && click_y < ty + th);
        s_drag_in_ruler   = (click_y >= ruler_y && click_y < ty);
        s_mm_seeking      = (click_y >= mm_y    && click_y < ruler_y);
        // Beat placement: only in content area, only when expanded
        s_drag_in_place   = (!s_beats_collapsed && click_x >= cx &&
                              click_y >= ps_y && click_y < ps_y + PLACE_STRIP_H);
        s_drag_in_beats   = (click_y >= ba_y    && click_y < ba_y + ba_h);
        s_drag_in_sec     = (click_y >= sa_y    && click_y < sa_y + sa_h);
        s_drag_in_lyr     = (click_y >= la_y    && click_y < la_y + la_h);

        // Any click outside the section strip clears section selection.
        if (!s_drag_in_sec)
            s_sec_selected = -1;
        // Any click outside the lyric strip clears lyric selection.
        if (!s_drag_in_lyr)
            s_lyr_selected = -1;

        if (s_drag_in_place) {
            double span = editor->view_end - editor->view_start;
            double t_place = (ps_w > 0.0f && span > 0.0)
                ? editor->view_start + (double)(io.MousePos.x - ps_x) / ps_w * span
                : editor->view_start;
            if (t_place < 0.0)              t_place = 0.0;
            if (t_place > editor->duration) t_place = editor->duration;
            undo_push(undo, beatmap);
            if (io.KeyShift && beatmap->count >= 1) {
                // Shift+click: add beat + fill from nearest beat using its instantaneous BPM.
                // Binary search for insertion point.
                int ins = 0;
                {
                    int lo2 = 0, hi2 = beatmap->count;
                    while (lo2 < hi2) {
                        int mid = (lo2 + hi2) / 2;
                        if (beatmap->beats[mid].time < t_place) lo2 = mid + 1;
                        else hi2 = mid;
                    }
                    ins = lo2;
                }
                double dist_l = (ins > 0)              ? t_place - beatmap->beats[ins-1].time : 1e18;
                double dist_r = (ins < beatmap->count) ? beatmap->beats[ins].time - t_place   : 1e18;
                int    near   = (dist_l <= dist_r) ? ins - 1 : ins;
                double fill_bpm = 0.0, fill_t1, fill_t2;
                if (near < ins) {
                    // Earlier beat is nearer: BPM = (Bn - Bn-1)
                    if (near > 0) {
                        double d = beatmap->beats[near].time - beatmap->beats[near-1].time;
                        if (d > 1e-6) fill_bpm = 60.0 / d;
                    }
                    fill_t1 = beatmap->beats[near].time;
                    fill_t2 = t_place;
                } else {
                    // Later beat is nearer: BPM = (Bn+1 - Bn)
                    if (near < beatmap->count - 1) {
                        double d = beatmap->beats[near+1].time - beatmap->beats[near].time;
                        if (d > 1e-6) fill_bpm = 60.0 / d;
                    }
                    fill_t1 = t_place;
                    fill_t2 = beatmap->beats[near].time;
                }
                beatmap_add(beatmap, t_place);
                if (fill_bpm > 0.0)
                    beatmap_fill(beatmap, fill_t1, fill_t2, fill_bpm);
            } else {
                beatmap_add(beatmap, t_place);
            }
        }

        if (s_drag_in_ruler && audio->loaded) {
            // Click on ruler seeks the playhead; dragging will pan as before.
            double span = editor->view_end - editor->view_start;
            double t = (cw > 0 && span > 0)
                ? editor->view_start + (io.MousePos.x - cx) / cw * span
                : editor->view_start;
            if (t < 0.0)              t = 0.0;
            if (t > editor->duration) t = editor->duration;
            audio_seek(audio, t);
        }

        if (s_drag_in_spectro) {
            double span = editor->view_end - editor->view_start;
            s_anchor = (tw > 0 && span > 0)
                ? editor->view_start + (io.MousePos.x - tx) / tw * span
                : editor->view_start;
            if (s_anchor < 0.0)              s_anchor = 0.0;
            if (s_anchor > editor->duration) s_anchor = editor->duration;
            editor->has_region = false;
            // A click on the spectrogram always seeks the playhead.
            // If the user then drags, a region is created on top of that.
            if (audio->loaded)
                audio_seek(audio, s_anchor);
        }

        if (s_drag_in_sec) {
            double span  = editor->view_end - editor->view_start;
            double t_raw = (sa_w > 0 && span > 0)
                ? editor->view_start + (double)(io.MousePos.x - sa_x) / sa_w * span
                : editor->view_start;
            double t_snap = snap_to_beat(t_raw, beatmap,
                                          editor->view_start, editor->view_end, sa_w);

            // Hit-test existing sections: check handles first, then body
            const float HANDLE_PX = 6.0f;
            int hit = -1, hit_end = 0;  // hit_end: 0=start, 1=end, 2=body
            for (int i = 0; i < sectionmap->count; i++) {
                float sx0 = time_to_x(sectionmap->sections[i].t_start,
                                       editor->view_start, editor->view_end, sa_x, sa_w);
                float sx1 = time_to_x(sectionmap->sections[i].t_end,
                                       editor->view_start, editor->view_end, sa_x, sa_w);
                float mx = io.MousePos.x;
                if (fabsf(mx - sx0) <= HANDLE_PX) { hit = i; hit_end = 0; break; }
                if (fabsf(mx - sx1) <= HANDLE_PX) { hit = i; hit_end = 1; break; }
                if (mx > sx0 && mx < sx1)          { hit = i; hit_end = 2; break; }
            }

            if (hit >= 0) {
                s_sec_selected = hit;
                s_sec_drag     = false;
                if (hit_end < 2) {
                    s_sec_hdrag     = true;
                    s_sec_hdrag_idx = hit;
                    s_sec_hdrag_end = hit_end;
                } else {
                    s_sec_hdrag = false;
                    // Double-click on section body → select it as the loop region
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        editor->has_region   = true;
                        editor->region_start = sectionmap->sections[hit].t_start;
                        editor->region_end   = sectionmap->sections[hit].t_end;
                    }
                }
            } else {
                s_sec_selected = -1;
                s_sec_drag     = true;
                s_sec_drag_t0  = t_snap;
                s_sec_drag_t1  = t_snap;
                s_sec_hdrag    = false;
            }
        }

        if (s_drag_in_lyr) {
            double span  = editor->view_end - editor->view_start;
            double t_raw = (la_w > 0 && span > 0)
                ? editor->view_start + (double)(io.MousePos.x - la_x) / la_w * span
                : editor->view_start;
            double t_snap = snap_to_beat(t_raw, beatmap,
                                          editor->view_start, editor->view_end, la_w);

            const float HANDLE_PX = 6.0f;
            int hit = -1, hit_end = 0;
            for (int i = 0; i < lyricmap->count; i++) {
                float lx0 = time_to_x(lyricmap->lyrics[i].t_start,
                                       editor->view_start, editor->view_end, la_x, la_w);
                float lx1 = time_to_x(lyricmap->lyrics[i].t_end,
                                       editor->view_start, editor->view_end, la_x, la_w);
                float mx = io.MousePos.x;
                if (fabsf(mx - lx0) <= HANDLE_PX) { hit = i; hit_end = 0; break; }
                if (fabsf(mx - lx1) <= HANDLE_PX) { hit = i; hit_end = 1; break; }
                if (mx > lx0 && mx < lx1)          { hit = i; hit_end = 2; break; }
            }

            if (hit >= 0) {
                s_lyr_selected = hit;
                s_lyr_drag     = false;
                if (hit_end < 2) {
                    s_lyr_hdrag     = true;
                    s_lyr_hdrag_idx = hit;
                    s_lyr_hdrag_end = hit_end;
                } else {
                    s_lyr_hdrag = false;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        editor->has_region   = true;
                        editor->region_start = lyricmap->lyrics[hit].t_start;
                        editor->region_end   = lyricmap->lyrics[hit].t_end;
                    }
                }
            } else {
                s_lyr_selected = -1;
                s_lyr_drag     = true;
                s_lyr_drag_t0  = t_snap;
                s_lyr_drag_t1  = t_snap;
                s_lyr_hdrag    = false;
            }
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

            {
                if (hit >= 0) {
                    int idx = s_vis[hit].idx;
                    if (io.KeyShift) {
                        // Shift+click: toggle selection, no drag
                        beatmap->beats[idx].selected = !beatmap->beats[idx].selected;
                        s_drag_beat = -1;
                    } else {
                        beatmap_clear_selection(beatmap);
                        beatmap->beats[idx].selected = true;
                        s_drag_beat  = idx;
                        s_drag_new_t = beatmap->beats[idx].time;
                        s_drag_dt    = s_drag_new_t - t_click;
                    }
                    s_rect_sel = false;
                } else {
                    if (!io.KeyShift) beatmap_clear_selection(beatmap);
                    s_drag_beat = -1;
                    s_rect_sel  = true;
                    s_rect_x0   = io.MousePos.x;
                    s_rect_y0   = io.MousePos.y;
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

    // Beat drag release: remove from old position, re-insert sorted at new position.
    // Preserve the interp flag so dragging doesn't accidentally commit a beat.
    if (s_drag_beat >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        bool was_interp   = beatmap->beats[s_drag_beat].interp;
        bool was_selected = beatmap->beats[s_drag_beat].selected;
        if (fabs(s_drag_new_t - beatmap->beats[s_drag_beat].time) > 1e-6)
            undo_push(undo, beatmap);
        beatmap_remove(beatmap, s_drag_beat);
        int new_idx = beatmap_add(beatmap, s_drag_new_t);
        if (new_idx >= 0) {
            beatmap->beats[new_idx].interp   = was_interp;
            beatmap->beats[new_idx].selected = was_selected;
        }
        s_drag_beat = -1;
    }

    // Section drag-to-create: update end time while mouse is held
    if (s_sec_drag && s_drag_in_sec &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        double span  = editor->view_end - editor->view_start;
        double t_raw = (sa_w > 0 && span > 0)
            ? editor->view_start + (double)(io.MousePos.x - sa_x) / sa_w * span
            : editor->view_start;
        s_sec_drag_t1 = snap_to_beat(t_raw, beatmap,
                                      editor->view_start, editor->view_end, sa_w);
    }

    // Section handle drag: resize selected section
    if (s_sec_hdrag && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (s_sec_hdrag_idx >= 0 && s_sec_hdrag_idx < sectionmap->count) {
            double span  = editor->view_end - editor->view_start;
            double t_raw = (sa_w > 0 && span > 0)
                ? editor->view_start + (double)(io.MousePos.x - sa_x) / sa_w * span
                : editor->view_start;
            double t_snap = snap_to_beat(t_raw, beatmap,
                                          editor->view_start, editor->view_end, sa_w);
            Section& s = sectionmap->sections[s_sec_hdrag_idx];
            if (s_sec_hdrag_end == 0)
                s.t_start = (t_snap < s.t_end - 0.001) ? t_snap : s.t_end - 0.001;
            else
                s.t_end   = (t_snap > s.t_start + 0.001) ? t_snap : s.t_start + 0.001;
            sectionmap->dirty = true;
        }
    }

    // Section drag-to-create release: commit new section
    if (s_sec_drag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        double pt0 = (s_sec_drag_t0 < s_sec_drag_t1) ? s_sec_drag_t0 : s_sec_drag_t1;
        double pt1 = (s_sec_drag_t0 < s_sec_drag_t1) ? s_sec_drag_t1 : s_sec_drag_t0;
        if (pt1 - pt0 > 0.05) {  // minimum 50 ms to prevent accidental tiny sections
            int idx = sectionmap_add(sectionmap, pt0, pt1, SK_VERSE, "");
            if (idx >= 0) s_sec_selected = idx;
        }
        s_sec_drag = false;
    }

    // Section handle drag release
    if (s_sec_hdrag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s_sec_hdrag     = false;
        s_sec_hdrag_idx = -1;
    }

    // Lyric drag-to-create: update end time while mouse is held
    if (s_lyr_drag && s_drag_in_lyr &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        double span  = editor->view_end - editor->view_start;
        double t_raw = (la_w > 0 && span > 0)
            ? editor->view_start + (double)(io.MousePos.x - la_x) / la_w * span
            : editor->view_start;
        s_lyr_drag_t1 = snap_to_beat(t_raw, beatmap,
                                      editor->view_start, editor->view_end, la_w);
    }

    // Lyric handle drag: resize selected lyric
    if (s_lyr_hdrag && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (s_lyr_hdrag_idx >= 0 && s_lyr_hdrag_idx < lyricmap->count) {
            double span  = editor->view_end - editor->view_start;
            double t_raw = (la_w > 0 && span > 0)
                ? editor->view_start + (double)(io.MousePos.x - la_x) / la_w * span
                : editor->view_start;
            double t_snap = snap_to_beat(t_raw, beatmap,
                                          editor->view_start, editor->view_end, la_w);
            Lyric& ly = lyricmap->lyrics[s_lyr_hdrag_idx];
            if (s_lyr_hdrag_end == 0)
                ly.t_start = (t_snap < ly.t_end - 0.001) ? t_snap : ly.t_end - 0.001;
            else
                ly.t_end   = (t_snap > ly.t_start + 0.001) ? t_snap : ly.t_start + 0.001;
            lyricmap->dirty = true;
        }
    }

    // Lyric drag-to-create release: commit new lyric
    if (s_lyr_drag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        double pt0 = (s_lyr_drag_t0 < s_lyr_drag_t1) ? s_lyr_drag_t0 : s_lyr_drag_t1;
        double pt1 = (s_lyr_drag_t0 < s_lyr_drag_t1) ? s_lyr_drag_t1 : s_lyr_drag_t0;
        if (pt1 - pt0 > 0.05) {
            int idx = lyricmap_add(lyricmap, pt0, pt1, "");
            if (idx >= 0) s_lyr_selected = idx;
        }
        s_lyr_drag = false;
    }

    // Lyric handle drag release
    if (s_lyr_hdrag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s_lyr_hdrag     = false;
        s_lyr_hdrag_idx = -1;
    }

    // Rect selection release: select all visible beats whose centre falls inside the rect
    if (s_rect_sel && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float rsx0 = s_rect_x0 < io.MousePos.x ? s_rect_x0 : io.MousePos.x;
        float rsx1 = s_rect_x0 < io.MousePos.x ? io.MousePos.x : s_rect_x0;
        float rsy0 = s_rect_y0 < io.MousePos.y ? s_rect_y0 : io.MousePos.y;
        float rsy1 = s_rect_y0 < io.MousePos.y ? io.MousePos.y : s_rect_y0;
        for (int i = 0; i < s_vis_n; i++) {
            if (s_vis[i].bx >= rsx0 && s_vis[i].bx <= rsx1 &&
                s_vis[i].cy >= rsy0 && s_vis[i].cy <= rsy1)
                beatmap->beats[s_vis[i].idx].selected = true;
        }
        s_rect_sel = false;
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s_drag_in_spectro = false;
        s_drag_in_ruler   = false;
        s_mm_seeking      = false;
        s_drag_in_place   = false;
        s_drag_in_beats   = false;
        s_drag_in_sec     = false;
        s_drag_in_lyr     = false;
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
        float pixel_frac = (cw > 0) ? (mouse_x - cx) / cw : 0.5f;
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
            if (cw > 0) editor_pan(editor, -dx / cw * span);
        }
    }

    // --- Autoscroll: pan the view to keep the playhead at the centre ---
    // Runs every frame while playing, after all manual input has been processed.
    if (editor->autoscroll && audio->loaded && audio->playing) {
        double pos  = audio_get_position(audio);
        double span = editor->view_end - editor->view_start;
        bool   do_pan    = true;
        double min_start = 0.0, max_start = 0.0;

        if (audio->loop) {
            double ls = editor->has_region ? editor->region_start : 0.0;
            double le = editor->has_region ? editor->region_end   : audio->duration;
            if (le - ls <= span) {
                do_pan = false;  // loop fits in view: let playhead advance freely
            } else {
                min_start = ls;
                max_start = le - span;
            }
        } else {
            min_start = 0.0;
            max_start = audio->duration - span;
            if (max_start < 0.0) max_start = 0.0;
        }

        if (do_pan) {
            double desired   = pos - span * 0.5;
            double new_start = desired < min_start ? min_start :
                               desired > max_start ? max_start : desired;
            editor->view_start = new_start;
            editor->view_end   = new_start + span;
        }
    }

    // --- Draw ---

    // Left sidebar: background + right border
    dl->AddRectFilled(ImVec2(rx, ry), ImVec2(cx, ry + total_h),
                      IM_COL32(12, 12, 18, 255));
    dl->AddLine(ImVec2(cx, ry), ImVec2(cx, ry + total_h),
                IM_COL32(50, 50, 70, 255));

    // Beat-editor collapse triangle in the left sidebar
    {
        float tcx = rx + sidebar_w * 0.5f;
        float tcy = ps_y + PLACE_STRIP_H * 0.5f;
        ImU32 tri_col = IM_COL32(160, 160, 190, 200);
        if (s_beats_collapsed) {
            // Right-pointing (▶): collapsed
            dl->AddTriangleFilled(ImVec2(tcx - 4.0f, tcy - 5.0f),
                                  ImVec2(tcx - 4.0f, tcy + 5.0f),
                                  ImVec2(tcx + 4.0f, tcy), tri_col);
        } else {
            // Down-pointing (▼): expanded
            dl->AddTriangleFilled(ImVec2(tcx - 5.0f, tcy - 3.5f),
                                  ImVec2(tcx + 5.0f, tcy - 3.5f),
                                  ImVec2(tcx,        tcy + 4.5f), tri_col);
        }
    }

    // Max-frequency +/- buttons in left sidebar at top of spectrogram (stacked vertically)
    {
        bool at_max = (s_spectro_max_khz >= 22);
        bool at_min = (s_spectro_max_khz <= 2);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 3.0f));
        float bw  = sidebar_w - 4.0f;
        float bh  = ImGui::GetFrameHeight();
        float bx  = rx + 2.0f;

        ImGui::SetCursorScreenPos(ImVec2(bx, ty + 2.0f));
        if (at_max) ImGui::BeginDisabled();
        if (ImGui::Button("+##mfp", ImVec2(bw, 0))) s_spectro_max_khz++;
        bool ph = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (at_max) ImGui::EndDisabled();
        if (ph) ImGui::SetTooltip("Max freq +1 kHz (now %d kHz)", s_spectro_max_khz);

        ImGui::SetCursorScreenPos(ImVec2(bx, ty + 2.0f + bh + 2.0f));
        if (at_min) ImGui::BeginDisabled();
        if (ImGui::Button("-##mfm", ImVec2(bw, 0))) s_spectro_max_khz--;
        bool mh = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (at_min) ImGui::EndDisabled();
        if (mh) ImGui::SetTooltip("Max freq -1 kHz (now %d kHz)", s_spectro_max_khz);

        ImGui::PopStyleVar();
    }

    // Frequency axis labels aligned to the spectrogram row
    if (spectro->computed && spectro->sample_rate > 0 && th > 0.0f) {
        float nyquist = (float)(spectro->sample_rate / 2);
        float max_freq_hz = (float)(s_spectro_max_khz * 1000);
        if (max_freq_hz > nyquist) max_freq_hz = nyquist;
        static const int   sb_freqs[] = {
            1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 10000, 12000, 16000, 20000 };
        static const char* sb_names[] = {
            "1k", "2k", "3k", "4k", "5k", "6k", "7k", "8k", "10k", "12k", "16k", "20k" };
        int   n_sb    = (int)(sizeof(sb_freqs) / sizeof(sb_freqs[0]));
        float lh      = ImGui::GetTextLineHeight();
        float last_bot = ty - lh - 2.0f;  // primed so the first label always passes
        for (int i = n_sb - 1; i >= 0; i--) {  // high-freq → low-freq  (top → bottom)
            if (sb_freqs[i] >= (int)max_freq_hz) continue;
            float frac    = 1.0f - (float)sb_freqs[i] / max_freq_hz;
            float cy_freq = ty + frac * th;
            float label_y = cy_freq - lh * 0.5f;
            if (label_y < ty) continue;
            if (label_y + lh > ty + th) continue;
            if (label_y < last_bot + 1.0f) continue;   // skip if overlapping
            ImVec2 ts = ImGui::CalcTextSize(sb_names[i]);
            dl->AddText(ImVec2(cx - ts.x - 4.0f, label_y),
                        IM_COL32(150, 150, 170, 160), sb_names[i]);
            // Short tick crossing the sidebar border
            dl->AddLine(ImVec2(cx - 3.0f, cy_freq), ImVec2(cx + 3.0f, cy_freq),
                        IM_COL32(150, 150, 170, 100));
            last_bot = label_y + lh;
        }
    }

    draw_minimap(dl, mm_x, mm_y, mm_w, mm_h,
                 editor->duration,
                 editor->view_start, editor->view_end,
                 editor->has_region, editor->region_start, editor->region_end,
                 audio->loaded, audio_get_position(audio));

    draw_ruler(dl, cx, ruler_y, cw, RULER_H,
               editor->view_start, editor->view_end);

    spectrogram_render(spectro, dl, tx, ty, tw, th,
                       editor->view_start, editor->view_end,
                       (float)(s_spectro_max_khz * 1000));

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

        // Selection duration label: one text-line below the cursor timestamp,
        // anchored just right of the (possibly clamped) selection start.
        if (cx2 > cx1) {
            char     dur_buf[32];
            snprintf(dur_buf, sizeof(dur_buf), "%.3fs",
                     editor->region_end - editor->region_start);
            ImVec2   sz    = ImGui::CalcTextSize(dur_buf);
            float    lbl_x = cx1 + 4.0f;
            float    lbl_y = ty  + 4.0f;
            if (lbl_x + sz.x > tx + tw) lbl_x = tx + tw - sz.x - 2.0f;
            if (lbl_y + sz.y <= ty + th)
                dl->AddText(ImVec2(lbl_x, lbl_y),
                            IM_COL32(130, 200, 255, 200), dur_buf);
        }
    }

    if (audio->loaded)
        draw_playhead(dl, tx, ty, tw, th,
                      audio_get_position(audio),
                      editor->view_start, editor->view_end);

    // Loop-end marker: shown when autoscroll + loop + loop doesn't fit in the view.
    // Drawn as an orange vertical line + downward triangle (same visual language as
    // the playhead) so the user can see where the view will jump back to loop_start.
    if (editor->autoscroll && audio->loop && audio->loaded) {
        double ls = editor->has_region ? editor->region_start : 0.0;
        double le = editor->has_region ? editor->region_end   : audio->duration;
        double span = editor->view_end - editor->view_start;
        if (le - ls > span) {
            float ex = time_to_x(le, editor->view_start, editor->view_end, tx, tw);
            if (ex >= tx && ex <= tx + tw) {
                dl->AddLine(ImVec2(ex, ruler_y), ImVec2(ex, ba_y + ba_h),
                            IM_COL32(255, 120, 40, 220), 2.0f);
                dl->AddTriangleFilled(ImVec2(ex - 5, ruler_y), ImVec2(ex + 5, ruler_y),
                                      ImVec2(ex, ruler_y + 10),
                                      IM_COL32(255, 120, 40, 220));
            }
        }
    }

    // Placement strip background
    dl->AddRectFilled(ImVec2(ps_x, ps_y), ImVec2(ps_x + ps_w, ps_y + PLACE_STRIP_H),
                      IM_COL32(12, 16, 22, 255));
    dl->AddRect(ImVec2(ps_x, ps_y), ImVec2(ps_x + ps_w, ps_y + PLACE_STRIP_H),
                IM_COL32(40, 50, 65, 255));
    dl->AddText(ImVec2(cx + 4.0f, ps_y + 3.0f), IM_COL32(90, 90, 110, 100), "Insert");

    // Condensed beat display (collapsed mode only) ----------------------------
    // Renders diamonds when beats are spread enough, lines when any adjacent pair
    // would overlap as diamonds (decision applies to the whole visible set).
    if (s_beats_collapsed && beatmap->count > 0) {
        const float CD_R    = 5.0f;  // diamond radius in collapsed strip
        float       strip_cy = ps_y + PLACE_STRIP_H * 0.5f;
        double      span     = editor->view_end - editor->view_start;

        // Pass 1: determine whether to use lines (overlap check on visible beats)
        bool  use_lines = false;
        float prev_bx   = -1e9f;
        for (int i = 0; i < beatmap->count && !use_lines; i++) {
            double t = beatmap->beats[i].time;
            if (t < editor->view_start || t > editor->view_end) continue;
            float bx = (span > 0.0 && ps_w > 0)
                ? ps_x + (float)((t - editor->view_start) / span * ps_w)
                : ps_x;
            if (prev_bx > -1e8f && bx - prev_bx < 2.0f * CD_R)
                use_lines = true;
            prev_bx = bx;
        }

        // Pass 2: draw
        dl->PushClipRect(ImVec2(ps_x, ps_y), ImVec2(ps_x + ps_w, ps_y + PLACE_STRIP_H), true);
        for (int i = 0; i < beatmap->count; i++) {
            double t = beatmap->beats[i].time;
            if (t < editor->view_start - 1e-6 || t > editor->view_end + 1e-6) continue;
            float bx = (span > 0.0 && ps_w > 0)
                ? ps_x + (float)((t - editor->view_start) / span * ps_w)
                : ps_x;
            bool  is_interp = beatmap->beats[i].interp;
            ImU32 fill   = is_interp ? IM_COL32(255, 190, 40, 130) : IM_COL32(255, 190, 40, 210);
            ImU32 border = is_interp ? IM_COL32(255, 220, 90, 180) : IM_COL32(255, 230, 100, 255);
            if (use_lines) {
                dl->AddLine(ImVec2(bx, ps_y + 2.0f), ImVec2(bx, ps_y + PLACE_STRIP_H - 2.0f),
                            fill, 1.0f);
            } else {
                draw_diamond(dl, bx, strip_cy, CD_R, fill, border);
            }
        }
        dl->PopClipRect();
    }

    // Placement strip hover preview: outline diamond + instantaneous BPM labels.
    // With Shift held: show fill preview (intermediate diamonds + spectrogram lines).
    if (hovered) {
        // Triangle tooltip
        if (io.MousePos.x < cx && io.MousePos.y >= ps_y && io.MousePos.y < ps_y + PLACE_STRIP_H)
            ImGui::SetTooltip(s_beats_collapsed ? "Expand beat editor" : "Collapse beat editor");

        float mpy = io.MousePos.y;
        if (!s_beats_collapsed && mpy >= ps_y && mpy < ps_y + PLACE_STRIP_H) {
            double span = editor->view_end - editor->view_start;
            double t_hover = (span > 0.0 && ps_w > 0.0f)
                ? editor->view_start + (double)(io.MousePos.x - ps_x) / ps_w * span
                : editor->view_start;
            if (t_hover >= 0.0 && t_hover <= editor->duration) {
                float phx = io.MousePos.x;
                float phy = ps_y + PLACE_STRIP_H * 0.5f;
                float r   = DIAMOND_R;

                // Binary search for insertion point (used in both branches)
                int ins = 0;
                {
                    int lo2 = 0, hi2 = beatmap->count;
                    while (lo2 < hi2) {
                        int mid = (lo2 + hi2) / 2;
                        if (beatmap->beats[mid].time < t_hover) lo2 = mid + 1;
                        else hi2 = mid;
                    }
                    ins = lo2;
                }

                if (io.KeyShift && beatmap->count >= 1) {
                    // --- Shift hover: fill preview ---
                    double dist_l = (ins > 0)              ? t_hover - beatmap->beats[ins-1].time : 1e18;
                    double dist_r = (ins < beatmap->count) ? beatmap->beats[ins].time - t_hover   : 1e18;
                    int    near   = (dist_l <= dist_r) ? ins - 1 : ins;
                    double fill_bpm = 0.0, fill_t1, fill_t2;
                    if (near < ins) {
                        if (near > 0) {
                            double d = beatmap->beats[near].time - beatmap->beats[near-1].time;
                            if (d > 1e-6) fill_bpm = 60.0 / d;
                        }
                        fill_t1 = beatmap->beats[near].time;
                        fill_t2 = t_hover;
                    } else {
                        if (near < beatmap->count - 1) {
                            double d = beatmap->beats[near+1].time - beatmap->beats[near].time;
                            if (d > 1e-6) fill_bpm = 60.0 / d;
                        }
                        fill_t1 = t_hover;
                        fill_t2 = beatmap->beats[near].time;
                    }
                    int n_fill = (fill_bpm > 0.0 && fill_t2 > fill_t1 + 1e-6)
                                 ? (int)round((fill_t2 - fill_t1) * fill_bpm / 60.0) : 0;

                    // Endpoint diamond in placement strip (highlighted to signal shift mode)
                    {
                        ImVec2 pts[4] = {
                            { phx,     phy - r },
                            { phx + r, phy     },
                            { phx,     phy + r },
                            { phx - r, phy     },
                        };
                        dl->AddPolyline(pts, 4, IM_COL32(160, 255, 200, 200),
                                        ImDrawFlags_Closed, 1.5f);
                    }

                    // Intermediate + endpoint preview in beat area and spectrogram
                    if (n_fill >= 2) {
                        // Spectrogram lines for all fill positions (k=1..n_fill-1) + endpoint
                        for (int k = 1; k < n_fill; k++) {
                            double t  = fill_t1 + (fill_t2 - fill_t1) * (double)k / n_fill;
                            float  sx = time_to_x(t, editor->view_start, editor->view_end, tx, tw);
                            if (sx >= tx && sx <= tx + tw)
                                dl->AddLine(ImVec2(sx, ty), ImVec2(sx, ty + th),
                                            IM_COL32(160, 255, 200, 70), 1.0f);
                        }
                        // Endpoint (t_hover) spectrogram line
                        if (phx >= tx && phx <= tx + tw)
                            dl->AddLine(ImVec2(phx, ty), ImVec2(phx, ty + th),
                                        IM_COL32(160, 255, 200, 70), 1.0f);

                        // Intermediate outline diamonds in beat area
                        dl->PushClipRect(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h), true);
                        for (int k = 1; k < n_fill; k++) {
                            double t   = fill_t1 + (fill_t2 - fill_t1) * (double)k / n_fill;
                            float  bx  = time_to_x(t, editor->view_start, editor->view_end, ba_x, ba_w);
                            float  cy  = ba_y + 0.5f * ba_h;
                            ImVec2 pts[4] = {
                                { bx,               cy - DIAMOND_R_INTERP },
                                { bx + DIAMOND_R_INTERP, cy               },
                                { bx,               cy + DIAMOND_R_INTERP },
                                { bx - DIAMOND_R_INTERP, cy               },
                            };
                            dl->AddPolyline(pts, 4, IM_COL32(160, 255, 200, 180),
                                            ImDrawFlags_Closed, 1.5f);
                        }
                        dl->PopClipRect();

                        // BPM label
                        char bpm_buf[32];
                        snprintf(bpm_buf, sizeof(bpm_buf), "%.1f bpm", fill_bpm);
                        ImVec2 ts = ImGui::CalcTextSize(bpm_buf);
                        float  lx = phx + r + 4.0f;
                        if (lx + ts.x <= ps_x + ps_w)
                            dl->AddText(ImVec2(lx, phy - ts.y * 0.5f),
                                        IM_COL32(160, 255, 200, 200), bpm_buf);
                    }
                } else {
                    // --- Normal hover: single outline diamond + instantaneous BPM labels ---
                    {
                        ImVec2 pts[4] = {
                            { phx,     phy - r },
                            { phx + r, phy     },
                            { phx,     phy + r },
                            { phx - r, phy     },
                        };
                        dl->AddPolyline(pts, 4, IM_COL32(140, 160, 180, 160),
                                        ImDrawFlags_Closed, 1.5f);
                    }
                    char bpm_buf[32];
                    ImVec2 ts;
                    if (ins > 0) {
                        double dt = t_hover - beatmap->beats[ins - 1].time;
                        if (dt > 1e-6) {
                            snprintf(bpm_buf, sizeof(bpm_buf), "%.1f", 60.0 / dt);
                            ts = ImGui::CalcTextSize(bpm_buf);
                            float lx = phx - r - 4.0f - ts.x;
                            if (lx >= ps_x)
                                dl->AddText(ImVec2(lx, phy - ts.y * 0.5f),
                                            IM_COL32(160, 200, 140, 200), bpm_buf);
                        }
                    }
                    if (ins < beatmap->count) {
                        double dt = beatmap->beats[ins].time - t_hover;
                        if (dt > 1e-6) {
                            snprintf(bpm_buf, sizeof(bpm_buf), "%.1f", 60.0 / dt);
                            ts = ImGui::CalcTextSize(bpm_buf);
                            float lx = phx + r + 4.0f;
                            if (lx + ts.x <= ps_x + ps_w)
                                dl->AddText(ImVec2(lx, phy - ts.y * 0.5f),
                                            IM_COL32(160, 200, 140, 200), bpm_buf);
                        }
                    }
                }
            }
        }
    }

    // Cursor time hint line + tooltip
    if (hovered) {
        float mx = io.MousePos.x;
        if (mx >= cx && mx <= cx + cw) {
            dl->AddLine(ImVec2(mx, ruler_y), ImVec2(mx, ty + th),
                        IM_COL32(255, 255, 255, 40), 1.0f);
            double hover_t = editor->view_start +
                             (mx - cx) / cw * (editor->view_end - editor->view_start);
            int hm = (int)(hover_t / 60.0);
            double hs = hover_t - hm * 60.0;
            char tip[32];
            snprintf(tip, sizeof(tip), "%d:%06.3f", hm, hs);
            float lh_hint = ImGui::GetTextLineHeight();
            dl->AddText(ImVec2(mx + 4, ty + 4.0f + lh_hint),
                        IM_COL32(255, 255, 255, 180), tip);
        }
    }

    // Beat area background
    dl->AddRectFilled(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h),
                      IM_COL32(14, 14, 22, 255));
    dl->AddRect(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h),
                IM_COL32(50, 50, 70, 255));
    if (!s_beats_collapsed)
        dl->AddText(ImVec2(cx + 4.0f, ba_y + 3.0f), IM_COL32(90, 90, 110, 100), "Beats");

    // Diamonds (clipped to beat area)
    dl->PushClipRect(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h), true);
    for (int i = 0; i < s_vis_n; i++) {
        int   idx       = s_vis[i].idx;
        bool  is_interp = beatmap->beats[idx].interp;
        bool  dragging  = (idx == s_drag_beat);
        bool  selected  = beatmap->beats[idx].selected;
        float r = is_interp ? DIAMOND_R_INTERP : DIAMOND_R;
        ImU32 fill, border;
        if (dragging) {
            fill   = IM_COL32( 80, 220, 100, 220);
            border = IM_COL32(140, 255, 160, 255);
        } else if (selected) {
            fill   = IM_COL32(100, 160, 255, 220);
            border = IM_COL32(180, 210, 255, 255);
        } else if (is_interp) {
            fill   = IM_COL32(255, 190,  40, 130);  // same hue, more transparent
            border = IM_COL32(255, 220,  90, 180);
        } else {
            fill   = IM_COL32(255, 190,  40, 210);
            border = IM_COL32(255, 230, 100, 255);
        }
        draw_diamond(dl, s_vis[i].bx, s_vis[i].cy, r, fill, border);
    }
    dl->PopClipRect();

    // Selection rect overlay
    if (s_rect_sel && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float rsx0 = s_rect_x0 < io.MousePos.x ? s_rect_x0 : io.MousePos.x;
        float rsx1 = s_rect_x0 < io.MousePos.x ? io.MousePos.x : s_rect_x0;
        float rsy0 = s_rect_y0 < io.MousePos.y ? s_rect_y0 : io.MousePos.y;
        float rsy1 = s_rect_y0 < io.MousePos.y ? io.MousePos.y : s_rect_y0;
        // Clamp to beat area
        if (rsx0 < ba_x)        rsx0 = ba_x;
        if (rsy0 < ba_y)        rsy0 = ba_y;
        if (rsx1 > ba_x + ba_w) rsx1 = ba_x + ba_w;
        if (rsy1 > ba_y + ba_h) rsy1 = ba_y + ba_h;
        if (rsx1 > rsx0 && rsy1 > rsy0) {
            dl->AddRectFilled(ImVec2(rsx0, rsy0), ImVec2(rsx1, rsy1),
                              IM_COL32(100, 160, 255, 40));
            dl->AddRect(ImVec2(rsx0, rsy0), ImVec2(rsx1, rsy1),
                        IM_COL32(140, 200, 255, 200), 0.0f, 0, 1.0f);
        }
    }

    // Hover BPM labels: instantaneous tempo to the left/right of the hovered beat
    if (hovered && s_drag_beat < 0) {
        float mpy = io.MousePos.y;
        if (mpy >= ba_y && mpy < ba_y + ba_h) {
            int   hv_vis  = -1;
            float hv_best = DIAMOND_R + 2.0f;
            for (int i = 0; i < s_vis_n; i++) {
                float dx = fabsf(io.MousePos.x - s_vis[i].bx);
                float dy = fabsf(io.MousePos.y - s_vis[i].cy);
                if (dx + dy <= hv_best) { hv_best = dx + dy; hv_vis = i; }
            }
            if (hv_vis >= 0) {
                int   idx  = s_vis[hv_vis].idx;
                float bx   = s_vis[hv_vis].bx;
                float cy   = s_vis[hv_vis].cy;
                float r    = beatmap->beats[idx].interp ? DIAMOND_R_INTERP : DIAMOND_R;
                char  buf[32];
                ImVec2 ts;

                if (idx > 0) {
                    double dt = beatmap->beats[idx].time - beatmap->beats[idx - 1].time;
                    if (dt > 1e-6) {
                        snprintf(buf, sizeof(buf), "%.1f", 60.0 / dt);
                        ts = ImGui::CalcTextSize(buf);
                        float lx = bx - r - 4.0f - ts.x;
                        if (lx >= ba_x)
                            dl->AddText(ImVec2(lx, cy - ts.y * 0.5f),
                                        IM_COL32(220, 220, 120, 220), buf);
                    }
                }
                if (idx < beatmap->count - 1) {
                    double dt = beatmap->beats[idx + 1].time - beatmap->beats[idx].time;
                    if (dt > 1e-6) {
                        snprintf(buf, sizeof(buf), "%.1f", 60.0 / dt);
                        ts = ImGui::CalcTextSize(buf);
                        float lx = bx + r + 4.0f;
                        if (lx + ts.x <= ba_x + ba_w)
                            dl->AddText(ImVec2(lx, cy - ts.y * 0.5f),
                                        IM_COL32(220, 220, 120, 220), buf);
                    }
                }
            }
        }
    }

    // --- Section strip ---
    // Drawn before ctx panel widget calls to avoid clip-rect restriction.
    dl->AddRectFilled(ImVec2(sa_x, sa_y), ImVec2(sa_x + sa_w, sa_y + sa_h),
                      IM_COL32(10, 10, 16, 255));
    dl->AddLine(ImVec2(sa_x, sa_y), ImVec2(sa_x + sa_w, sa_y),
                IM_COL32(40, 40, 60, 255));
    dl->AddText(ImVec2(cx + 4.0f, sa_y + 3.0f), IM_COL32(90, 90, 110, 100), "Sections");

    dl->PushClipRect(ImVec2(sa_x, sa_y), ImVec2(sa_x + sa_w, sa_y + sa_h), true);
    for (int i = 0; i < sectionmap->count; i++) {
        const Section& sec = sectionmap->sections[i];
        float sx0 = time_to_x(sec.t_start, editor->view_start, editor->view_end, sa_x, sa_w);
        float sx1 = time_to_x(sec.t_end,   editor->view_start, editor->view_end, sa_x, sa_w);
        if (sx1 <= sa_x || sx0 >= sa_x + sa_w) continue;
        bool  sel = (i == s_sec_selected);
        float y0  = sa_y + 2.0f, y1 = sa_y + sa_h - 2.0f;

        dl->AddRectFilled(ImVec2(sx0, y0), ImVec2(sx1, y1), s_sec_fill[sec.kind]);
        dl->AddRect(ImVec2(sx0, y0), ImVec2(sx1, y1),
                    sel ? s_sec_border[sec.kind] : IM_COL32(0, 0, 0, 120),
                    0.0f, 0, sel ? 2.0f : 1.0f);

        // Measure ticks: binary-search for first beat in section, then count by ts_num
        {
            int bi;
            {
                int lo = 0, hi = beatmap->count;
                while (lo < hi) {
                    int m = (lo + hi) / 2;
                    if (beatmap->beats[m].time < sec.t_start) lo = m + 1;
                    else hi = m;
                }
                bi = lo;
            }
            int   beat_in_sec = 0;
            int   meas_num    = 1;
            float tick_top    = y0 + 1.0f;
            float tick_bot    = y1 - 1.0f;
            float num_y       = y0 + 2.0f;  // measure number sits at top of rect

            for (int b = bi; b < beatmap->count && beatmap->beats[b].time <= sec.t_end; b++) {
                if (beat_in_sec % sec.ts_num == 0) {
                    float bx = time_to_x(beatmap->beats[b].time,
                                          editor->view_start, editor->view_end, sa_x, sa_w);
                    // Tick line (skip the very first beat — coincides with section left edge)
                    if (bx > sx0 + 1.0f && bx < sx1 - 1.0f)
                        dl->AddLine(ImVec2(bx, tick_top), ImVec2(bx, tick_bot),
                                    IM_COL32(255, 255, 255, 55), 1.0f);
                    // Measure number
                    char mbuf[8];
                    snprintf(mbuf, sizeof(mbuf), "%d", meas_num);
                    float num_x = bx + 2.0f;
                    if (num_x >= sx0 && num_x < sx1 - 2.0f)
                        dl->AddText(ImVec2(num_x, num_y),
                                    IM_COL32(255, 255, 255, 130), mbuf);
                    meas_num++;
                }
                beat_in_sec++;
            }
        }

        // Kind name or custom label — biased toward the lower half to leave room for
        // measure numbers at the top
        const char* lbl   = sec.label[0] ? sec.label : SECTION_KIND_NAMES[sec.kind];
        float       lbl_x = sx0 + 4.0f;
        float       th_lbl = ImGui::GetTextLineHeight();
        float       lbl_y = y0 + (y1 - y0) * 0.62f - th_lbl * 0.5f;
        ImVec2      lbl_s = ImGui::CalcTextSize(lbl);
        if (lbl_x + lbl_s.x < sx1 - 2.0f)
            dl->AddText(ImVec2(lbl_x, lbl_y), IM_COL32(255, 255, 255, 220), lbl);

        // Resize handles on selected section
        if (sel) {
            float hmy = y0 + (y1 - y0) * 0.5f;
            dl->AddRectFilled(ImVec2(sx0 - 3.0f, hmy - 8.0f),
                              ImVec2(sx0 + 3.0f, hmy + 8.0f), s_sec_border[sec.kind]);
            dl->AddRectFilled(ImVec2(sx1 - 3.0f, hmy - 8.0f),
                              ImVec2(sx1 + 3.0f, hmy + 8.0f), s_sec_border[sec.kind]);
        }
    }
    // Drag-to-create preview
    if (s_sec_drag) {
        double dt0 = (s_sec_drag_t0 < s_sec_drag_t1) ? s_sec_drag_t0 : s_sec_drag_t1;
        double dt1 = (s_sec_drag_t0 < s_sec_drag_t1) ? s_sec_drag_t1 : s_sec_drag_t0;
        float  dx0 = time_to_x(dt0, editor->view_start, editor->view_end, sa_x, sa_w);
        float  dx1 = time_to_x(dt1, editor->view_start, editor->view_end, sa_x, sa_w);
        if (dx1 > dx0) {
            dl->AddRectFilled(ImVec2(dx0, sa_y + 2.0f), ImVec2(dx1, sa_y + sa_h - 2.0f),
                              IM_COL32(120, 180, 255, 60));
            dl->AddRect(ImVec2(dx0, sa_y + 2.0f), ImVec2(dx1, sa_y + sa_h - 2.0f),
                        IM_COL32(160, 210, 255, 200), 0.0f, 0, 1.5f);
        }
    }
    dl->PopClipRect();


    // --- Lyric strip ---
    dl->AddRectFilled(ImVec2(la_x, la_y), ImVec2(la_x + la_w, la_y + la_h),
                      IM_COL32(10, 14, 18, 255));
    dl->AddLine(ImVec2(la_x, la_y), ImVec2(la_x + la_w, la_y),
                IM_COL32(40, 40, 60, 255));
    dl->AddText(ImVec2(cx + 4.0f, la_y + 3.0f), IM_COL32(90, 90, 110, 100), "Lyrics");

    dl->PushClipRect(ImVec2(la_x, la_y), ImVec2(la_x + la_w, la_y + la_h), true);
    for (int i = 0; i < lyricmap->count; i++) {
        const Lyric& ly = lyricmap->lyrics[i];
        float lx0 = time_to_x(ly.t_start, editor->view_start, editor->view_end, la_x, la_w);
        float lx1 = time_to_x(ly.t_end,   editor->view_start, editor->view_end, la_x, la_w);
        if (lx1 <= la_x || lx0 >= la_x + la_w) continue;
        bool  sel = (i == s_lyr_selected);
        float y0  = la_y + 3.0f, y1 = la_y + la_h - 3.0f;

        dl->AddRectFilled(ImVec2(lx0, y0), ImVec2(lx1, y1),
                          IM_COL32(45, 95, 130, 160));
        dl->AddRect(ImVec2(lx0, y0), ImVec2(lx1, y1),
                    sel ? IM_COL32(90, 180, 220, 230) : IM_COL32(0, 0, 0, 120),
                    0.0f, 0, sel ? 2.0f : 1.0f);

        // Lyric text, left-aligned inside the rect
        if (ly.text[0]) {
            float th_lbl = ImGui::GetTextLineHeight();
            float lbl_y  = y0 + (y1 - y0 - th_lbl) * 0.5f;
            float lbl_x  = lx0 + 4.0f;
            ImVec2 ts = ImGui::CalcTextSize(ly.text);
            // Clip text to lyric rect width
            if (lbl_x + ts.x < lx1 - 2.0f)
                dl->AddText(ImVec2(lbl_x, lbl_y), IM_COL32(220, 220, 200, 230), ly.text);
            else if (lx1 - lx0 > 10.0f) {
                // Ellipsis: push clip rect for the individual lyric
                dl->PushClipRect(ImVec2(lx0, y0), ImVec2(lx1 - 2.0f, y1), true);
                dl->AddText(ImVec2(lbl_x, lbl_y), IM_COL32(220, 220, 200, 230), ly.text);
                dl->PopClipRect();
            }
        }

        // Resize handles on selected lyric
        if (sel) {
            float hmy = y0 + (y1 - y0) * 0.5f;
            dl->AddRectFilled(ImVec2(lx0 - 3.0f, hmy - 8.0f),
                              ImVec2(lx0 + 3.0f, hmy + 8.0f), IM_COL32(90, 180, 220, 230));
            dl->AddRectFilled(ImVec2(lx1 - 3.0f, hmy - 8.0f),
                              ImVec2(lx1 + 3.0f, hmy + 8.0f), IM_COL32(90, 180, 220, 230));
        }
    }
    // Drag-to-create preview
    if (s_lyr_drag) {
        double dt0 = (s_lyr_drag_t0 < s_lyr_drag_t1) ? s_lyr_drag_t0 : s_lyr_drag_t1;
        double dt1 = (s_lyr_drag_t0 < s_lyr_drag_t1) ? s_lyr_drag_t1 : s_lyr_drag_t0;
        float  dx0 = time_to_x(dt0, editor->view_start, editor->view_end, la_x, la_w);
        float  dx1 = time_to_x(dt1, editor->view_start, editor->view_end, la_x, la_w);
        if (dx1 > dx0) {
            dl->AddRectFilled(ImVec2(dx0, la_y + 3.0f), ImVec2(dx1, la_y + la_h - 3.0f),
                              IM_COL32(90, 160, 200, 60));
            dl->AddRect(ImVec2(dx0, la_y + 3.0f), ImVec2(dx1, la_y + la_h - 3.0f),
                        IM_COL32(120, 200, 240, 200), 0.0f, 0, 1.5f);
        }
    }
    dl->PopClipRect();

    // --- Unified edit panel background --- always drawn to keep layout stable.
    dl->AddRectFilled(ImVec2(cx, ep_y), ImVec2(cx + cw, ep_y + EDIT_PANEL_H),
                      IM_COL32(10, 10, 18, 255));
    dl->AddLine(ImVec2(cx, ep_y), ImVec2(cx + cw, ep_y),
                IM_COL32(50, 50, 70, 255));
    if (s_sec_selected < 0 && s_lyr_selected < 0)
        dl->AddText(ImVec2(cx + 4.0f, ep_y + 3.0f), IM_COL32(90, 90, 110, 100), "Edit");

    // --- Contextual interpolation panel ---
    // Shown when exactly two adjacent beats are selected.
    if (show_ctx) {
        double t1 = beatmap->beats[ctx_sel[0]].time;
        double t2 = beatmap->beats[ctx_sel[1]].time;
        double dt = t2 - t1;

        // Reset BPM/count when the selected pair changes
        if (ctx_sel[0] != s_ctx_prev0 || ctx_sel[1] != s_ctx_prev1) {
            s_ctx_prev0 = ctx_sel[0];
            s_ctx_prev1 = ctx_sel[1];

            // Instantaneous BPM from the interval immediately outside each selected beat
            double bpm_left = 0.0, bpm_right = 0.0;
            if (ctx_sel[0] > 0) {
                double d = beatmap->beats[ctx_sel[0]].time
                         - beatmap->beats[ctx_sel[0] - 1].time;
                if (d > 1e-6) bpm_left = 60.0 / d;
            }
            if (ctx_sel[1] < beatmap->count - 1) {
                double d = beatmap->beats[ctx_sel[1] + 1].time
                         - beatmap->beats[ctx_sel[1]].time;
                if (d > 1e-6) bpm_right = 60.0 / d;
            }

            // Average instantaneous BPM across the whole beatmap (plausible range only)
            double avg_bpm = 0.0;
            int    avg_n   = 0;
            for (int k = 0; k < beatmap->count - 1; k++) {
                double d = beatmap->beats[k + 1].time - beatmap->beats[k].time;
                if (d > 1e-6) {
                    double b = 60.0 / d;
                    if (b >= 50.0 && b <= 250.0) { avg_bpm += b; avg_n++; }
                }
            }
            if (avg_n > 0) avg_bpm /= avg_n;
            else           avg_bpm  = 120.0;

            // Pick the candidate within [50, 250] BPM closest to the average
            bool left_ok  = (bpm_left  >= 50.0 && bpm_left  <= 250.0);
            bool right_ok = (bpm_right >= 50.0 && bpm_right <= 250.0);
            double suggested;
            if (left_ok && right_ok)
                suggested = (fabs(bpm_left  - avg_bpm) <= fabs(bpm_right - avg_bpm))
                            ? bpm_left : bpm_right;
            else if (left_ok)  suggested = bpm_left;
            else if (right_ok) suggested = bpm_right;
            else               suggested = avg_bpm;   // fallback: global average

            // Derive count, then snap BPM so it's exact for the integer count
            int n = (int)round(dt * suggested / 60.0);
            s_ctx_count = (n > 1) ? n - 1 : 1;
            s_ctx_bpm   = (dt > 1e-6)
                          ? (float)(60.0 * (s_ctx_count + 1) / dt)
                          : (float)suggested;
        }

        // Panel background
        float ctx_y = ba_y + ba_h;
        dl->AddRectFilled(ImVec2(ba_x, ctx_y), ImVec2(ba_x + ba_w, ctx_y + CTX_PANEL_H),
                          IM_COL32(10, 10, 18, 255));
        dl->AddLine(ImVec2(ba_x, ctx_y), ImVec2(ba_x + ba_w, ctx_y),
                    IM_COL32(50, 50, 70, 255));

        // Connector lines from the two selected beats down to the panel
        float x0 = time_to_x(t1, editor->view_start, editor->view_end, ba_x, ba_w);
        float x1 = time_to_x(t2, editor->view_start, editor->view_end, ba_x, ba_w);
        float mid_x = (x0 + x1) * 0.5f;
        dl->AddLine(ImVec2(x0, ba_y + ba_h), ImVec2(mid_x, ctx_y + CTX_PANEL_H * 0.3f),
                    IM_COL32(100, 160, 255, 60), 1.0f);
        dl->AddLine(ImVec2(x1, ba_y + ba_h), ImVec2(mid_x, ctx_y + CTX_PANEL_H * 0.3f),
                    IM_COL32(100, 160, 255, 60), 1.0f);

        // Widgets — position them centred under the midpoint between the two beats
        float fh      = ImGui::GetFrameHeight();
        float bpm_w   = 88.0f;
        float count_w = 72.0f;
        float btn_w   = 46.0f;
        float sp      = ImGui::GetStyle().ItemSpacing.x;
        float panel_w = bpm_w + sp + count_w + sp + btn_w;
        float px      = mid_x - panel_w * 0.5f;
        if (px < ba_x + 4)             px = ba_x + 4;
        if (px + panel_w > ba_x + ba_w - 4) px = ba_x + ba_w - 4 - panel_w;
        float py = ctx_y + (CTX_PANEL_H - fh) * 0.5f;

        // Preview drawn before widget calls so the clip rect is still the full
        // child-window rect (widget calls restrict it to the ctx panel region).
        // Uses previous-frame s_ctx_hover; the 1-frame lag is imperceptible.
        if (s_ctx_hover && s_ctx_count >= 1) {
            int n = s_ctx_count + 1;  // number of gaps
            // Vertical lines on spectrogram — manual bounds check, no clip push
            // (matches pattern of region lines and cursor hint drawn on the spectrogram)
            for (int k = 1; k < n; k++) {
                double t  = t1 + (t2 - t1) * (double)k / n;
                float  sx = time_to_x(t, editor->view_start, editor->view_end, tx, tw);
                if (sx >= tx && sx <= tx + tw)
                    dl->AddLine(ImVec2(sx, ty), ImVec2(sx, ty + th),
                                IM_COL32(160, 210, 255, 90), 1.0f);
            }
            // Outline diamonds in beat area
            dl->PushClipRect(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h), true);
            for (int k = 1; k < n; k++) {
                double t   = t1 + (t2 - t1) * (double)k / n;
                float  bx  = time_to_x(t, editor->view_start, editor->view_end, ba_x, ba_w);
                float  cy  = ba_y + 0.5f * ba_h;
                ImVec2 pts[4] = {
                    { bx,               cy - DIAMOND_R },
                    { bx + DIAMOND_R,   cy             },
                    { bx,               cy + DIAMOND_R },
                    { bx - DIAMOND_R,   cy             },
                };
                dl->AddPolyline(pts, 4, IM_COL32(160, 210, 255, 180),
                                ImDrawFlags_Closed, 1.5f);
            }
            dl->PopClipRect();
        }

        ImGui::SetCursorScreenPos(ImVec2(px, py));

        // BPM drag — editing BPM recomputes count
        ImGui::SetNextItemWidth(bpm_w);
        if (ImGui::DragFloat("##ctx_bpm", &s_ctx_bpm, 0.5f, 1.0f, 9999.0f, "%.1f bpm")) {
            if (s_ctx_bpm < 1.0f) s_ctx_bpm = 1.0f;
            int n = (int)round(dt * s_ctx_bpm / 60.0);
            s_ctx_count = (n > 1) ? n - 1 : 1;
            // Snap BPM so it's exact for the count
            s_ctx_bpm = (dt > 1e-6) ? (float)(60.0 * (s_ctx_count + 1) / dt) : s_ctx_bpm;
        }
        ImGui::SameLine();

        // Count drag — editing count recomputes BPM
        ImGui::SetNextItemWidth(count_w);
        if (ImGui::DragInt("##ctx_count", &s_ctx_count, 0.2f, 1, 999, "%d beats")) {
            if (s_ctx_count < 1) s_ctx_count = 1;
            s_ctx_bpm = (dt > 1e-6) ? (float)(60.0 * (s_ctx_count + 1) / dt) : s_ctx_bpm;
        }
        ImGui::SameLine();

        if (ImGui::Button("Fill##ctx", ImVec2(btn_w, 0))) {
            undo_push(undo, beatmap);
            beatmap_fill(beatmap, t1, t2, (double)s_ctx_bpm);
        }
        s_ctx_hover = ImGui::IsItemHovered();
    } else {
        // Pair is no longer selected — invalidate cached indices
        s_ctx_prev0 = s_ctx_prev1 = -1;
        s_ctx_hover = false;
    }

    // --- Unified edit panel widgets ---
    // Shows section or lyric controls depending on which is selected.
    {
        float fh  = ImGui::GetFrameHeight();
        float btn_w = 58.0f;
        float py  = ep_y + (EDIT_PANEL_H - fh) * 0.5f;

        if (s_sec_selected >= 0 && s_sec_selected < sectionmap->count) {
            Section& sec = sectionmap->sections[s_sec_selected];
            float combo_w = 120.0f;
            float label_w = 140.0f;

            ImGui::SetCursorScreenPos(ImVec2(cx + 4.0f, py));

            // Kind combo
            ImGui::SetNextItemWidth(combo_w);
            int kind_int = (int)sec.kind;
            if (ImGui::BeginCombo("##sec_kind", SECTION_KIND_NAMES[kind_int])) {
                for (int k = 0; k < SK_COUNT; k++) {
                    bool sel_k = (k == kind_int);
                    if (ImGui::Selectable(SECTION_KIND_NAMES[k], sel_k)) {
                        sec.kind          = (SectionKind)k;
                        sectionmap->dirty = true;
                    }
                    if (sel_k) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();

            // Optional custom label
            ImGui::SetNextItemWidth(label_w);
            if (ImGui::InputText("##sec_label", sec.label, sizeof(sec.label)))
                sectionmap->dirty = true;
            ImGui::SameLine();

            // Time signature: numerator DragInt + "/" + denominator DragInt
            ImGui::SetNextItemWidth(32.0f);
            if (ImGui::DragInt("##ts_num", &sec.ts_num, 0.15f, 1, 16, "%d")) {
                if (sec.ts_num < 1) sec.ts_num = 1;
                sectionmap->dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Beats per measure");
            ImGui::SameLine(0, 2.0f);
            ImGui::TextUnformatted("/");
            ImGui::SameLine(0, 2.0f);
            ImGui::SetNextItemWidth(32.0f);
            if (ImGui::DragInt("##ts_den", &sec.ts_den, 0.15f, 1, 32, "%d")) {
                if (sec.ts_den < 1) sec.ts_den = 1;
                sectionmap->dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Note value (4 = quarter note)");

            // Delete button — right-aligned
            ImGui::SameLine(ImGui::GetWindowWidth() - btn_w - 4.0f);
            if (ImGui::Button("Delete##sec", ImVec2(btn_w, 0))) {
                sectionmap_remove(sectionmap, s_sec_selected);
                s_sec_selected = -1;
            }

        } else if (s_lyr_selected >= 0 && s_lyr_selected < lyricmap->count) {
            Lyric& ly = lyricmap->lyrics[s_lyr_selected];
            float text_w = 320.0f;

            ImGui::SetCursorScreenPos(ImVec2(cx + 4.0f, py));

            ImGui::TextUnformatted("Lyric:");
            ImGui::SameLine();

            ImGui::SetNextItemWidth(text_w);
            if (ImGui::InputText("##lyr_text", ly.text, sizeof(ly.text)))
                lyricmap->dirty = true;

            // Delete button — right-aligned
            ImGui::SameLine(ImGui::GetWindowWidth() - btn_w - 4.0f);
            if (ImGui::Button("Delete##lyr", ImVec2(btn_w, 0))) {
                lyricmap_remove(lyricmap, s_lyr_selected);
                s_lyr_selected = -1;
            }
        }
    }

    ImGui::EndChild();
}
