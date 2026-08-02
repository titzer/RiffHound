#include "ui_timeline.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "miscmap.h"
#include "beat_algo.h"
#include "ui_beat_detector.h"
#include "ui_smoothing.h"
#include "panels.h"
#include "undo.h"
#include "imgui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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

// Snap a paste anchor to the nearest beat when it lands within `frac` of one.
// snap_to_beat()'s pixel tolerance means nothing for a keyboard paste, and an
// anchor deliberately placed mid-beat (beyond frac) is still honoured.
static double snap_anchor_to_beat(double t, const BeatMap* bm, double frac = 0.35) {
    if (bm->count < 2) return t;
    double b  = beatmap_beat_pos(bm, t);
    double nb = floor(b + 0.5);
    if (fabs(b - nb) > frac) return t;
    return beatmap_time_at(bm, nb);
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

// Snap time t to the nearest onset in ab within the given window (seconds).
// Returns t unchanged when no onset is close enough.
static double snap_to_onset(double t, const AutoBeatList* ab, double window)
{
    if (!ab || ab->onset_count == 0) return t;
    double best_dist = window;
    double best_t    = t;
    for (int i = 0; i < ab->onset_count; i++) {
        double d = fabs(ab->onset_times[i] - t);
        if (d < best_dist) { best_dist = d; best_t = ab->onset_times[i]; }
    }
    return best_t;
}

// Vertical placement for the hover BPM labels.  Drawn on the marker's own row
// they cover the neighbouring beats the tempo is measured against, so lift them
// clear of the marker: above when there is room, otherwise below, and in a strip
// too short for either, hug whichever edge has more space.
static float bpm_label_y(float cy, float marker_r, float text_h,
                         float strip_top, float strip_bot)
{
    const float PAD = 2.0f;
    float above = cy - marker_r - PAD - text_h;
    if (above >= strip_top + 1.0f) return above;
    float below = cy + marker_r + PAD;
    if (below + text_h <= strip_bot - 1.0f) return below;
    return (cy - strip_top >= strip_bot - cy) ? strip_top + 1.0f
                                              : strip_bot - 1.0f - text_h;
}

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

// --- tempo graph --------------------------------------------------------
// Drawn behind the beat markers.  Each inter-beat interval contributes one
// horizontal tick at its instantaneous BPM (deliberately not joined into a line
// graph, so individual outliers stand out); a rolling average is overlaid as a
// darker yellow line.

// Reads beat times from either a BeatMap or a plain array (smoothing preview).
struct TimeSeq {
    const Beat*   beats;
    const double* times;
    int           n;
    double at(int i) const { return beats ? beats[i].time : times[i]; }
};

static float bpm_to_y(double bpm, float gy, float gh, float min_bpm, float max_bpm) {
    double range = (double)max_bpm - (double)min_bpm;
    if (range <= 1e-6) return gy + gh;
    double frac = (bpm - min_bpm) / range;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    return gy + gh * (float)(1.0 - frac);
}

// Instantaneous BPM: one horizontal tick spanning each interval.
// Intervals whose tempo falls outside [min_bpm, max_bpm] are clamped to the
// edge and drawn in the clipped colour so they are not mistaken for in-range.
static void draw_tempo_ticks(ImDrawList* dl, const TimeSeq& seq,
                             float gx, float gy, float gw, float gh,
                             double view_start, double view_end,
                             float min_bpm, float max_bpm,
                             ImU32 col, ImU32 clip_col, float thick)
{
    for (int i = 1; i < seq.n; i++) {
        double t0 = seq.at(i - 1), t1 = seq.at(i);
        if (t1 < view_start || t0 > view_end) continue;
        double dt = t1 - t0;
        if (dt <= 1e-9) continue;
        double bpm = 60.0 / dt;

        float x0 = time_to_x(t0, view_start, view_end, gx, gw);
        float x1 = time_to_x(t1, view_start, view_end, gx, gw);
        if (x0 < gx)      x0 = gx;
        if (x1 > gx + gw) x1 = gx + gw;
        if (x1 <= x0) x1 = x0 + 1.0f;

        bool  clipped = (bpm < min_bpm || bpm > max_bpm);
        float y = bpm_to_y(bpm, gy, gh, min_bpm, max_bpm);
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), clipped ? clip_col : col, thick);
    }
}

// Rolling average of the instantaneous BPM over `window` intervals, plotted as
// a line through the midpoint of each interval.
static void draw_tempo_average(ImDrawList* dl, const TimeSeq& seq,
                               float gx, float gy, float gw, float gh,
                               double view_start, double view_end,
                               float min_bpm, float max_bpm,
                               int window, ImU32 col, float thick)
{
    if (seq.n < 3) return;
    if (window < 2) window = 2;
    int half = window / 2;

    bool   have_prev = false;
    ImVec2 prev(0, 0);
    for (int i = 1; i < seq.n; i++) {
        double t0 = seq.at(i - 1), t1 = seq.at(i);
        // Keep one interval on each side of the view so the line reaches the
        // borders instead of stopping at the first visible beat.
        if (t1 < view_start && i + 1 < seq.n && seq.at(i + 1) < view_start) {
            have_prev = false;
            continue;
        }
        bool past_right = (t0 > view_end);

        double sum = 0.0;
        int    cnt = 0;
        for (int k = i - half; k <= i + half; k++) {
            if (k < 1 || k >= seq.n) continue;
            double d = seq.at(k) - seq.at(k - 1);
            if (d > 1e-9) { sum += 60.0 / d; cnt++; }
        }
        if (cnt == 0) { have_prev = false; if (past_right) break; continue; }

        float xm = time_to_x((t0 + t1) * 0.5, view_start, view_end, gx, gw);
        float ym = bpm_to_y(sum / cnt, gy, gh, min_bpm, max_bpm);
        ImVec2 cur(xm, ym);
        if (have_prev) dl->AddLine(prev, cur, col, thick);
        if (past_right) break;   // this segment reached the right edge
        prev      = cur;
        have_prev = true;
    }
}

// Data labels: the instantaneous BPM of every interval, centred over the
// interval.  Labels are thinned so they never overlap each other, which keeps
// the display readable when zoomed out.  With the tempo graph on they ride just
// above their tick; otherwise they sit clear of the marker row.
static void draw_bpm_labels(ImDrawList* dl, const TimeSeq& seq,
                            float gx, float gy, float gw, float gh,
                            double view_start, double view_end,
                            bool on_graph, float min_bpm, float max_bpm,
                            float marker_r, ImU32 col)
{
    float text_h   = ImGui::GetTextLineHeight();
    float last_right = -1e9f;

    for (int i = 1; i < seq.n; i++) {
        double t0 = seq.at(i - 1), t1 = seq.at(i);
        if (t1 < view_start || t0 > view_end) continue;
        double dt = t1 - t0;
        if (dt <= 1e-9) continue;
        double bpm = 60.0 / dt;

        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", bpm);
        ImVec2 ts = ImGui::CalcTextSize(buf);

        float xm = time_to_x((t0 + t1) * 0.5, view_start, view_end, gx, gw);
        float lx = xm - ts.x * 0.5f;
        if (lx < last_right + 3.0f) continue;      // would collide with the previous label
        if (lx < gx || lx + ts.x > gx + gw) continue;

        float ly;
        if (on_graph) {
            ly = bpm_to_y(bpm, gy, gh, min_bpm, max_bpm) - text_h - 1.0f;
            if (ly < gy + 1.0f)            ly = bpm_to_y(bpm, gy, gh, min_bpm, max_bpm) + 1.0f;
            if (ly + text_h > gy + gh - 1.0f) ly = gy + gh - 1.0f - text_h;
        } else {
            ly = bpm_label_y(gy + gh * 0.5f, marker_r, text_h, gy, gy + gh);
        }

        dl->AddText(ImVec2(lx, ly), col, buf);
        last_right = lx + ts.x;
    }
}

// Background scale: horizontal gridlines every `step` BPM with right-aligned labels.
static void draw_tempo_scale(ImDrawList* dl, float gx, float gy, float gw, float gh,
                             float min_bpm, float max_bpm)
{
    float range = max_bpm - min_bpm;
    if (range <= 0.0f) return;
    float step = (range <= 60.0f) ? 10.0f : (range <= 150.0f) ? 25.0f : 50.0f;

    for (float b = ceilf(min_bpm / step) * step; b < max_bpm; b += step) {
        if (b <= min_bpm + 0.01f) continue;
        float y = bpm_to_y(b, gy, gh, min_bpm, max_bpm);
        dl->AddLine(ImVec2(gx, y), ImVec2(gx + gw, y), IM_COL32(90, 90, 115, 55), 1.0f);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", b);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(gx + gw - ts.x - 3.0f, y - ts.y * 0.5f),
                    IM_COL32(140, 140, 170, 90), buf);
    }
}

// --- layout constants --------------------------------------------------

// ---- Lyric font size control --------------------------------------------

static ImFont** s_lyric_fonts      = nullptr;
static int      s_lyric_font_count = 0;
static int      s_lyric_font_idx   = 0;

void ui_timeline_set_lyric_fonts(ImFont** fonts, int count, int default_idx) {
    s_lyric_fonts      = fonts;
    s_lyric_font_count = count;
    s_lyric_font_idx   = (default_idx >= 0 && default_idx < count) ? default_idx : 0;
}
void ui_timeline_lyric_font_larger()  { if (s_lyric_font_idx < s_lyric_font_count - 1) s_lyric_font_idx++; }
void ui_timeline_lyric_font_smaller() { if (s_lyric_font_idx > 0) s_lyric_font_idx--; }
bool ui_timeline_lyric_font_can_grow()   { return s_lyric_font_idx < s_lyric_font_count - 1; }
bool ui_timeline_lyric_font_can_shrink() { return s_lyric_font_idx > 0; }

static ImFont* s_lyric_font() {
    if (s_lyric_fonts && s_lyric_font_count > 0)
        return s_lyric_fonts[s_lyric_font_idx];
    return ImGui::GetFont();
}

// ---- Lyric split callback -----------------------------------------------
// Shared state written by the callback, read by the site that called InputText.
struct LyrSplitState { bool req; int cursor; };
static LyrSplitState s_lyr_split_state;

static int lyr_split_callback(ImGuiInputTextCallbackData* d) {
    if (ImGui::IsKeyDown(ImGuiMod_Shift) &&
            ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        s_lyr_split_state.req    = true;
        s_lyr_split_state.cursor = d->CursorPos;
    }
    return 0;
}

static const float RULER_H       = 24.0f;
static const float MINIMAP_H     = 40.0f;
static const float CTX_PANEL_H   = 36.0f;  // contextual interpolate panel
static const float PLACE_STRIP_H = 22.0f;  // beat placement strip
static const float TAP_STRIP_H       = 22.0f;  // tap recording strip
static const float AUTOBEAT_STRIP_H  = 22.0f;  // auto-detected beat strip
static const float SECTION_H         = 52.0f;  // section strip
static const float LYRIC_H       = 36.0f;  // lyric strip
static const float MISC_STRIP_H  = 36.0f;  // misc annotation strip

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

// --- Tap strip data ---
static const int MAX_TAPS = 1024;
struct TapEntry { double time; bool selected; };
static TapEntry s_taps[MAX_TAPS];
static int      s_tap_count = 0;

// --- main widget -------------------------------------------------------

void ui_timeline_render(EditorState* editor, AudioState* audio,
                        SpectrogramState* spectro, BeatMap* beatmap,
                        UndoStack* undo, SectionMap* sectionmap,
                        LyricMap* lyricmap, MiscMap* miscmap,
                        AutoBeatList* autobeat)
{
    ImGuiIO& io = ImGui::GetIO();

    static bool s_beats_collapsed   = false;  // beat editor collapsed to slim display strip
    static int  s_spectro_max_khz  = 22;     // max displayed frequency [2, 22] kHz
    static bool s_spectro_log      = false;  // logarithmic frequency axis
    static bool s_lyric_index_open = false;  // lyric index floating window visible

    // Strip visibility is read once through the panel registry so that layout,
    // hit-testing and drawing all agree for the whole frame.
    const bool show_place = panel_visible(editor, PANEL_INSERT);
    const bool show_beats = panel_visible(editor, PANEL_BEATS);
    const bool show_tempo = panel_visible(editor, PANEL_TEMPO);
    const bool show_taps  = panel_visible(editor, PANEL_TAPS);
    const bool show_auto  = panel_visible(editor, PANEL_AUTO);
    const bool show_sect  = panel_visible(editor, PANEL_SECTIONS);
    const bool show_lyr   = panel_visible(editor, PANEL_LYRICS);
    const bool show_misc  = panel_visible(editor, PANEL_MISC);

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

    // Dynamic layout: only count visible strips in fixed_h.
    // If beat strip is hidden, suppress the ctx panel too.
    if (!show_beats) { show_ctx = false; ctx_h = 0.0f; }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float strips_h = 2.0f;  // initial gap between spectrogram and first strip
    if (show_place) strips_h += PLACE_STRIP_H    + 2.0f;
    if (show_taps)  strips_h += TAP_STRIP_H      + 2.0f;
    if (show_auto)  strips_h += AUTOBEAT_STRIP_H + 2.0f;
    if (show_beats && !s_beats_collapsed) strips_h += BEAT_AREA_H;
    strips_h += ctx_h;
    if (show_sect) strips_h += 2.0f + SECTION_H;
    if (show_lyr)  strips_h += 2.0f + LYRIC_H;
    if (show_misc) strips_h += 2.0f + MISC_STRIP_H;

    // Pane checkboxes live in a narrow lane on the left, bottom-aligned with the
    // timeline.  They normally sit alongside the strips and cost no extra height;
    // only when the visible strips are shorter than the column does the timeline
    // reserve the difference, so the column never rides up over the spectrogram.
    const float PANE_ROW_H = ImGui::GetFrameHeight() + 3.0f;
    const float PANE_COL_H = PANE_ROW_H * panels_count(PK_STRIP);
    if (strips_h < PANE_COL_H) strips_h = PANE_COL_H;

    float fixed_h = MINIMAP_H + 2.0f + RULER_H + 2.0f + strips_h;
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

    // Left lane holds the pane checkboxes; next to it the sidebar is just wide
    // enough to show "20k" with 4 px padding on each side.
    float pane_lane_w = ImGui::GetFrameHeight() + 6.0f;
    float sidebar_w   = pane_lane_w + ImGui::CalcTextSize("20k").x + 8.0f;
    float sb_x        = rx + pane_lane_w;  // sidebar (labels / buttons) left edge
    float sb_w        = sidebar_w - pane_lane_w;
    float cx = rx + sidebar_w;   // content area left edge
    float cw = rw - sidebar_w;   // content area width

    float mm_x = cx, mm_y = ry,                       mm_w = cw, mm_h = MINIMAP_H;
    float ruler_y = mm_y + mm_h + 2.0f;
    float tx = cx,   ty = ruler_y + RULER_H + 2.0f,   tw = cw,   th = spectro_h;

    // Running-y: compute strip top positions based on which strips are visible.
    float _y = ty + th + 2.0f;

    float ps_x = cx, ps_w = cw, ps_y = _y;
    if (show_place) _y += PLACE_STRIP_H + 2.0f;

    float tap_x = cx, tap_w = cw, tap_y = _y;
    if (show_taps) _y += TAP_STRIP_H + 2.0f;

    float ab_x = cx, ab_w = cw, ab_y = _y;
    if (show_auto) _y += AUTOBEAT_STRIP_H + 2.0f;

    float ba_x = cx, ba_w = cw, ba_y = _y;
    float ba_h = (show_beats && !s_beats_collapsed) ? BEAT_AREA_H : 0.0f;
    if (show_beats && !s_beats_collapsed) _y += BEAT_AREA_H;
    float ctx_y = ba_y + ba_h;
    _y += ctx_h;

    float sa_x = cx, sa_w = cw, sa_h = SECTION_H;
    if (show_sect) _y += 2.0f;
    float sa_y = _y;
    if (show_sect) _y += SECTION_H;

    float la_x = cx, la_w = cw, la_h = LYRIC_H;
    if (show_lyr) _y += 2.0f;
    float la_y = _y;
    if (show_lyr) _y += LYRIC_H;

    float misc_x = cx, misc_w = cw;
    if (show_misc) _y += 2.0f;
    float misc_y = _y;
    if (show_misc) _y += MISC_STRIP_H;

    // Pane checkbox column: left lane, bottom-aligned with the timeline.
    float pane_col_y = ry + total_h - PANE_COL_H;

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

    // --- Single InvisibleButton covering the whole timeline ---
    // AllowOverlap so that widgets added later in the frame (sidebar buttons,
    // pane checkboxes, interpolate-panel controls) still receive hover and
    // clicks.  It spans the full height: trimming it by the interpolate panel's
    // height used to leave a dead band at the bottom where the lower strips
    // stopped responding whenever that panel was open.
    ImGui::SetNextItemAllowOverlap();
    ImGui::SetCursorScreenPos(ImVec2(rx, ry));
    ImGui::InvisibleButton("##timeline_input",
                           ImVec2(rw, total_h),
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
    static bool   s_drag_in_tap      = false;  // drag started in tap strip
    static bool   s_drag_in_autobeat = false;  // drag started in auto-beat strip
    // Tap selection state
    static bool   s_tap_rect_sel    = false;
    static float  s_tap_rect_x0     = 0.0f;
    // Autobeat selection state
    static bool   s_ab_rect_sel     = false;
    static float  s_ab_rect_x0      = 0.0f;
    // Section editing state
    static bool   s_sec_drag        = false;   // drag-to-create in progress
    static double s_sec_drag_t0     = 0.0;
    static double s_sec_drag_t1     = 0.0;
    static bool   s_sec_hdrag       = false;   // handle resize drag in progress
    static int    s_sec_hdrag_idx   = -1;      // which section is handle-dragged
    static int    s_sec_hdrag_end   = 0;       // 0 = start handle, 1 = end handle
    static double s_sec_hdrag_t0    = 0.0;     // pre-drag bounds, to detect no-ops
    static double s_sec_hdrag_t1    = 0.0;
    static int    s_sec_kind_popup  = -1;      // section whose kind picker is open
    // Lyric editing state
    static bool   s_lyr_drag        = false;   // drag-to-create in progress
    static double s_lyr_drag_t0     = 0.0;
    static double s_lyr_drag_t1     = 0.0;
    static bool   s_lyr_hdrag       = false;   // handle resize drag in progress
    static int    s_lyr_hdrag_idx   = -1;
    static int    s_lyr_hdrag_end   = 0;
    static double s_lyr_hdrag_t0    = 0.0;     // pre-drag bounds, to detect no-ops
    static double s_lyr_hdrag_t1    = 0.0;
    static bool   s_lyr_body_drag     = false; // whole-lyric translate drag
    static int    s_lyr_body_drag_idx = -1;
    static double s_lyr_body_drag_dt  = 0.0;   // cursor_t - t_start at drag start
    static double s_lyr_body_drag_dur = 0.0;   // lyric duration preserved during drag
    static double s_lyr_body_new_t0   = 0.0;   // virtual t_start while dragging
    static int    s_lyr_inline_edit   = -1;     // lyric index being inline-edited (-1 = none)
    // Misc strip editing state
    static bool   s_drag_in_misc      = false;
    static bool   s_misc_drag         = false;  // drag-to-create in progress
    static double s_misc_drag_t0      = 0.0;
    static double s_misc_drag_t1      = 0.0;
    static bool   s_misc_hdrag        = false;
    static int    s_misc_hdrag_idx    = -1;
    static int    s_misc_hdrag_end    = 0;
    static double s_misc_hdrag_t0     = 0.0;   // pre-drag bounds, to detect no-ops
    static double s_misc_hdrag_t1     = 0.0;
    static int    s_misc_selected     = -1;
    static bool   s_misc_rect_sel     = false;  // shift+drag rubber-band select
    static float  s_misc_rect_x0      = 0.0f;
    static bool   s_misc_body_drag    = false;  // translate the selection
    static double s_misc_body_grab    = 0.0;    // cursor_t - t_start at grab
    static double s_misc_body_t0      = 0.0;    // grabbed entry's t_start at grab
    static double s_misc_body_delta   = 0.0;    // applied to the whole selection
    static int    s_misc_sync         = -1;     // last value stamped into the map
    // undo_pop() clears selected_idx behind our back; adopt an outside change
    // instead of stamping a stale focus index back over it.
    if (miscmap->selected_idx != s_misc_sync) s_misc_selected = miscmap->selected_idx;
    if (s_misc_selected >= miscmap->count) s_misc_selected = -1;
    miscmap->selected_idx = s_misc_sync = s_misc_selected;  // keep struct in sync
    static int    s_misc_inline_edit  = -1;     // misc index being inline-edited
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

        // Triangle in left sidebar: toggle beat editor collapse (eats the click).
        // Only live while the insertion strip — which owns that row — is shown.
        if (show_place && click_x >= sb_x && click_x < cx &&
            click_y >= ps_y && click_y < ps_y + PLACE_STRIP_H)
            s_beats_collapsed = !s_beats_collapsed;

        s_drag_in_spectro = (click_x >= cx && click_y >= ty && click_y < ty + th);
        s_drag_in_ruler   = (click_y >= ruler_y && click_y < ty);
        s_mm_seeking      = (click_y >= mm_y    && click_y < ruler_y);
        // Beat placement: only when the strip is visible and expanded, and only
        // in the content area.  Without the visibility test a hidden strip would
        // still swallow clicks (its row collapses onto the next strip).
        s_drag_in_place   = (show_place && !s_beats_collapsed && click_x >= cx &&
                              click_y >= ps_y && click_y < ps_y + PLACE_STRIP_H);
        s_drag_in_tap      = (show_taps && click_x >= cx &&
                               click_y >= tap_y && click_y < tap_y + TAP_STRIP_H);
        s_drag_in_autobeat = (show_auto && autobeat && click_x >= cx &&
                               click_y >= ab_y && click_y < ab_y + AUTOBEAT_STRIP_H);
        s_drag_in_beats   = (show_beats &&
                              click_y >= ba_y    && click_y < ba_y + ba_h);
        s_drag_in_sec     = (show_sect &&
                              click_y >= sa_y    && click_y < sa_y + sa_h);
        s_drag_in_lyr     = (show_lyr &&
                              click_y >= la_y    && click_y < la_y + la_h);
        s_drag_in_misc    = (show_misc && click_x >= cx &&
                              click_y >= misc_y  && click_y < misc_y + MISC_STRIP_H);

        // Any click outside the section strip clears section selection.
        if (!s_drag_in_sec)
            s_sec_selected = -1;
        // Any click outside the lyric strip clears lyric selection and inline edit.
        if (!s_drag_in_lyr) {
            s_lyr_selected    = -1;
            s_lyr_inline_edit = -1;
        }
        // Any click outside the misc strip clears misc selection and inline
        // edit.  The clipboard is unaffected, so copy-here / seek / paste-there
        // survives the seek click.
        if (!s_drag_in_misc) {
            miscmap_clear_selection(miscmap);
            s_misc_selected    = -1;
            s_misc_inline_edit = -1;
        }

        if (s_drag_in_place) {
            double span = editor->view_end - editor->view_start;
            double t_place = (ps_w > 0.0f && span > 0.0)
                ? editor->view_start + (double)(io.MousePos.x - ps_x) / ps_w * span
                : editor->view_start;
            if (t_place < 0.0)              t_place = 0.0;
            if (t_place > editor->duration) t_place = editor->duration;
            undo_push(undo, beatmap, lyricmap);
            int count_before = beatmap->count;
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
                // Snap the clicked position itself to nearest onset if feature is on.
                if (editor->snap_interp_to_onsets && fill_bpm > 0.0) {
                    // Ensure onsets cover the fill range; run detection if needed.
                    ui_beat_detector_ensure_onsets(audio, beatmap, autobeat,
                                                   fill_t1, fill_t2);
                    t_place = snap_to_onset(t_place, autobeat, 0.20 * 60.0 / fill_bpm);
                }
                beatmap_add(beatmap, t_place);
                if (fill_bpm > 0.0) {
                    if (editor->snap_interp_to_onsets) {
                        // Inline fill with per-position onset snapping.
                        double snap_win = 0.20 * 60.0 / fill_bpm;
                        int n = (int)round((fill_t2 - fill_t1) * fill_bpm / 60.0);
                        for (int k = 1; k < n; k++) {
                            double gt = fill_t1 + (fill_t2 - fill_t1) * k / n;
                            gt = snap_to_onset(gt, autobeat, snap_win);
                            int idx = beatmap_add(beatmap, gt);
                            if (idx >= 0) beatmap->beats[idx].interp = true;
                        }
                    } else {
                        beatmap_fill(beatmap, fill_t1, fill_t2, fill_bpm);
                    }
                }
            } else {
                beatmap_add(beatmap, t_place);
            }
            // A click too close to an existing beat adds nothing; don't leave a
            // do-nothing entry on the undo stack.
            if (beatmap->count == count_before) undo_drop_last(undo);
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
                    // Snapshot before the resize; dropped again on release if the
                    // handle never actually moved.
                    s_sec_hdrag_t0  = sectionmap->sections[hit].t_start;
                    s_sec_hdrag_t1  = sectionmap->sections[hit].t_end;
                    undo_push(undo, nullptr, nullptr, sectionmap, nullptr);
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
            float mx = io.MousePos.x;

            // Pass 1: selected lyric's handles have priority so they remain clickable
            // even when another lyric abuts or overlaps at the same screen position.
            if (s_lyr_selected >= 0 && s_lyr_selected < lyricmap->count) {
                float lx0 = time_to_x(lyricmap->lyrics[s_lyr_selected].t_start,
                                       editor->view_start, editor->view_end, la_x, la_w);
                float lx1 = time_to_x(lyricmap->lyrics[s_lyr_selected].t_end,
                                       editor->view_start, editor->view_end, la_x, la_w);
                if      (fabsf(mx - lx0) <= HANDLE_PX) { hit = s_lyr_selected; hit_end = 0; }
                else if (fabsf(mx - lx1) <= HANDLE_PX) { hit = s_lyr_selected; hit_end = 1; }
            }
            // Pass 2: no priority handle hit — scan all lyrics (handles then bodies)
            if (hit < 0) {
                for (int i = 0; i < lyricmap->count; i++) {
                    float lx0 = time_to_x(lyricmap->lyrics[i].t_start,
                                           editor->view_start, editor->view_end, la_x, la_w);
                    float lx1 = time_to_x(lyricmap->lyrics[i].t_end,
                                           editor->view_start, editor->view_end, la_x, la_w);
                    if (fabsf(mx - lx0) <= HANDLE_PX) { hit = i; hit_end = 0; break; }
                    if (fabsf(mx - lx1) <= HANDLE_PX) { hit = i; hit_end = 1; break; }
                    if (mx > lx0 && mx < lx1)          { hit = i; hit_end = 2; break; }
                }
            }

            if (hit >= 0) {
                s_lyr_selected = hit;
                s_lyr_drag     = false;
                if (hit_end < 2) {
                    // Handle drag (resize)
                    s_lyr_hdrag         = true;
                    s_lyr_hdrag_idx     = hit;
                    s_lyr_hdrag_end     = hit_end;
                    s_lyr_body_drag     = false;
                    s_lyr_body_drag_idx = -1;
                    s_lyr_hdrag_t0      = lyricmap->lyrics[hit].t_start;
                    s_lyr_hdrag_t1      = lyricmap->lyrics[hit].t_end;
                    undo_push(undo, nullptr, lyricmap);
                } else {
                    // Body click: arm a translate drag.  A click that never
                    // moves is a no-op on release, so arming costs nothing and
                    // a lyric stays draggable after it has been selected.
                    s_lyr_hdrag             = false;
                    s_lyr_body_drag         = true;
                    s_lyr_body_drag_idx     = hit;
                    s_lyr_body_drag_dur     = lyricmap->lyrics[hit].t_end
                                             - lyricmap->lyrics[hit].t_start;
                    s_lyr_body_drag_dt      = t_raw - lyricmap->lyrics[hit].t_start;
                    s_lyr_body_new_t0       = lyricmap->lyrics[hit].t_start;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        // Double-click: enter inline text edit
                        s_lyr_inline_edit = hit;
                        s_lyr_body_drag   = false;
                        s_lyr_body_drag_idx = -1;
                    }
                }
            } else {
                s_lyr_selected      = -1;
                s_lyr_drag          = true;
                s_lyr_drag_t0       = t_snap;
                s_lyr_drag_t1       = t_snap;
                s_lyr_hdrag         = false;
                s_lyr_body_drag     = false;
                s_lyr_body_drag_idx = -1;
            }
        }

        if (s_drag_in_tap) {
            // Hit-test taps
            int   hit  = -1;
            float best = 8.0f;
            for (int i = 0; i < s_tap_count; i++) {
                float bx = time_to_x(s_taps[i].time,
                                     editor->view_start, editor->view_end, tap_x, tap_w);
                float dx = fabsf(io.MousePos.x - bx);
                if (dx < best) { best = dx; hit = i; }
            }
            if (hit >= 0) {
                if (io.KeyShift) {
                    s_taps[hit].selected = !s_taps[hit].selected;
                } else {
                    for (int i = 0; i < s_tap_count; i++) s_taps[i].selected = false;
                    s_taps[hit].selected = true;
                }
                s_tap_rect_sel = false;
            } else {
                if (!io.KeyShift)
                    for (int i = 0; i < s_tap_count; i++) s_taps[i].selected = false;
                s_tap_rect_sel = true;
                s_tap_rect_x0  = io.MousePos.x;
            }
        }

        if (s_drag_in_autobeat && autobeat) {
            // Hit-test auto-beats
            int   hit  = -1;
            float best = 8.0f;
            for (int i = 0; i < autobeat->beat_count; i++) {
                float bx = time_to_x(autobeat->beat_times[i],
                                     editor->view_start, editor->view_end, ab_x, ab_w);
                float dx = fabsf(io.MousePos.x - bx);
                if (dx < best) { best = dx; hit = i; }
            }
            if (hit >= 0) {
                if (io.KeyShift) {
                    autobeat->beat_selected[hit] = !autobeat->beat_selected[hit];
                } else {
                    for (int i = 0; i < autobeat->beat_count; i++)
                        autobeat->beat_selected[i] = false;
                    autobeat->beat_selected[hit] = true;
                }
                s_ab_rect_sel = false;
            } else {
                if (!io.KeyShift)
                    for (int i = 0; i < autobeat->beat_count; i++)
                        autobeat->beat_selected[i] = false;
                s_ab_rect_sel = true;
                s_ab_rect_x0  = io.MousePos.x;
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

        if (s_drag_in_misc) {
            double span  = editor->view_end - editor->view_start;
            double t_raw = (misc_w > 0 && span > 0)
                ? editor->view_start + (double)(io.MousePos.x - misc_x) / misc_w * span
                : editor->view_start;
            double t_snap = snap_to_beat(t_raw, beatmap,
                                          editor->view_start, editor->view_end, misc_w);
            const float HANDLE_PX = 6.0f;
            int hit = -1, hit_end = 0;
            float mx = io.MousePos.x;
            for (int i = 0; i < miscmap->count; i++) {
                float mx0 = time_to_x(miscmap->entries[i].t_start,
                                       editor->view_start, editor->view_end, misc_x, misc_w);
                float mx1 = time_to_x(miscmap->entries[i].t_end,
                                       editor->view_start, editor->view_end, misc_x, misc_w);
                if (fabsf(mx - mx0) <= HANDLE_PX) { hit = i; hit_end = 0; break; }
                if (fabsf(mx - mx1) <= HANDLE_PX) { hit = i; hit_end = 1; break; }
                if (mx > mx0 && mx < mx1)          { hit = i; hit_end = 2; break; }
            }
            if (hit >= 0) {
                s_misc_drag = false;
                if (io.KeyShift) {
                    // Shift+click toggles group membership -- no edge drag, no
                    // inline edit, so a group can be assembled click by click.
                    bool on = !miscmap->entries[hit].selected;
                    miscmap->entries[hit].selected = on;
                    s_misc_selected    = on ? hit : -1;
                    s_misc_inline_edit = -1;
                    s_misc_hdrag       = false;
                    s_misc_body_drag   = false;
                } else {
                    // Clicking a member keeps the group (so it can be copied
                    // right after); clicking a non-member replaces it.
                    if (!miscmap->entries[hit].selected) {
                        miscmap_clear_selection(miscmap);
                        miscmap->entries[hit].selected = true;
                    }
                    s_misc_selected = hit;
                    if (hit_end < 2) {
                        s_misc_hdrag     = true;
                        s_misc_hdrag_idx = hit;
                        s_misc_hdrag_end = hit_end;
                        s_misc_hdrag_t0  = miscmap->entries[hit].t_start;
                        s_misc_hdrag_t1  = miscmap->entries[hit].t_end;
                        s_misc_body_drag = false;
                        undo_push(undo, nullptr, nullptr, nullptr, miscmap);
                    } else {
                        // Body: arm a translate drag of the whole selection.
                        // A click that never moves is a no-op on release.
                        s_misc_hdrag      = false;
                        s_misc_body_drag  = true;
                        s_misc_body_t0    = miscmap->entries[hit].t_start;
                        s_misc_body_grab  = t_raw - s_misc_body_t0;
                        s_misc_body_delta = 0.0;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            s_misc_inline_edit = hit;
                            s_misc_body_drag   = false;
                        }
                    }
                }
            } else if (io.KeyShift) {
                // Shift+drag over empty strip rubber-bands a group instead of
                // creating an annotation.
                s_misc_rect_sel    = true;
                s_misc_rect_x0     = io.MousePos.x;
                s_misc_selected    = -1;
                s_misc_inline_edit = -1;
                s_misc_drag        = false;
                s_misc_hdrag       = false;
                s_misc_body_drag   = false;
            } else {
                miscmap_clear_selection(miscmap);
                s_misc_selected    = -1;
                s_misc_inline_edit = -1;
                s_misc_drag        = true;
                s_misc_drag_t0     = t_snap;
                s_misc_drag_t1     = t_snap;
                s_misc_hdrag       = false;
                s_misc_body_drag   = false;
            }
        }
    }

    // Right-click on a section: open the kind picker for it.
    if (hovered && show_sect && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        io.MousePos.y >= sa_y && io.MousePos.y < sa_y + sa_h) {
        float mx  = io.MousePos.x;
        int   hit = -1;
        for (int i = 0; i < sectionmap->count; i++) {
            float sx0 = time_to_x(sectionmap->sections[i].t_start,
                                   editor->view_start, editor->view_end, sa_x, sa_w);
            float sx1 = time_to_x(sectionmap->sections[i].t_end,
                                   editor->view_start, editor->view_end, sa_x, sa_w);
            if (mx >= sx0 && mx <= sx1) { hit = i; break; }
        }
        if (hit >= 0) {
            s_sec_selected   = hit;
            s_sec_kind_popup = hit;
            ImGui::OpenPopup("##sec_kind");
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
            undo_push(undo, beatmap, lyricmap);
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
            undo_push(undo, nullptr, nullptr, sectionmap, nullptr);
            int idx = sectionmap_add(sectionmap, pt0, pt1, SK_VERSE, "");
            if (idx >= 0) s_sec_selected = idx;
            else          undo_drop_last(undo);
        }
        s_sec_drag = false;
    }

    // Section handle drag release
    if (s_sec_hdrag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // A click that selected a handle without moving it leaves no undo entry.
        if (s_sec_hdrag_idx >= 0 && s_sec_hdrag_idx < sectionmap->count) {
            const Section& s = sectionmap->sections[s_sec_hdrag_idx];
            if (fabs(s.t_start - s_sec_hdrag_t0) < 1e-9 &&
                fabs(s.t_end   - s_sec_hdrag_t1) < 1e-9)
                undo_drop_last(undo);
        }
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
            undo_push(undo, beatmap, lyricmap);
            int idx = lyricmap_add(lyricmap, pt0, pt1, "");
            if (idx >= 0) s_lyr_selected = idx;
        }
        s_lyr_drag = false;
    }

    // Lyric handle drag release
    if (s_lyr_hdrag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (s_lyr_hdrag_idx >= 0 && s_lyr_hdrag_idx < lyricmap->count) {
            const Lyric& ly = lyricmap->lyrics[s_lyr_hdrag_idx];
            if (fabs(ly.t_start - s_lyr_hdrag_t0) < 1e-9 &&
                fabs(ly.t_end   - s_lyr_hdrag_t1) < 1e-9)
                undo_drop_last(undo);
        }
        s_lyr_hdrag     = false;
        s_lyr_hdrag_idx = -1;
    }

    // Lyric body drag: translate the whole segment while mouse is held
    if (s_lyr_body_drag && s_drag_in_lyr &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        double span     = editor->view_end - editor->view_start;
        double t_cursor = (la_w > 0 && span > 0)
            ? editor->view_start + (double)(io.MousePos.x - la_x) / la_w * span
            : editor->view_start;
        // Snap the leading edge, as the resize drags do -- a lyric that lands
        // a few ms off its beat reads as sloppy in every client downstream.
        double new_t0 = snap_to_beat(t_cursor - s_lyr_body_drag_dt, beatmap,
                                      editor->view_start, editor->view_end, la_w);
        if (new_t0 < 0.0) new_t0 = 0.0;
        if (new_t0 + s_lyr_body_drag_dur > editor->duration)
            new_t0 = editor->duration - s_lyr_body_drag_dur;
        s_lyr_body_new_t0 = new_t0;
    }

    // Lyric body drag release: re-sort at new position
    if (s_lyr_body_drag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        int i = s_lyr_body_drag_idx;
        if (i >= 0 && i < lyricmap->count &&
            fabs(s_lyr_body_new_t0 - lyricmap->lyrics[i].t_start) > 1e-6) {
            undo_push(undo, beatmap, lyricmap);
            double t0 = s_lyr_body_new_t0;
            double t1 = t0 + s_lyr_body_drag_dur;
            char   saved[128];
            strncpy(saved, lyricmap->lyrics[i].text, sizeof(saved) - 1);
            saved[sizeof(saved) - 1] = '\0';
            lyricmap_remove(lyricmap, i);
            int ni = lyricmap_add(lyricmap, t0, t1, saved);
            s_lyr_selected         = ni;
            lyricmap->selected_idx = ni;
        }
        s_lyr_body_drag     = false;
        s_lyr_body_drag_idx = -1;
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

    // Misc drag-to-create: update end time while dragging
    if (s_misc_drag && s_drag_in_misc &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        double span  = editor->view_end - editor->view_start;
        double t_raw = (misc_w > 0 && span > 0)
            ? editor->view_start + (double)(io.MousePos.x - misc_x) / misc_w * span
            : editor->view_start;
        s_misc_drag_t1 = snap_to_beat(t_raw, beatmap,
                                       editor->view_start, editor->view_end, misc_w);
    }

    // Misc body drag: translate the whole selection while the mouse is held.
    // The map itself is untouched until release -- moving in place would
    // re-sort the array under the indices this drag is holding.
    if (s_misc_body_drag && s_drag_in_misc &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        double span     = editor->view_end - editor->view_start;
        double t_cursor = (misc_w > 0 && span > 0)
            ? editor->view_start + (double)(io.MousePos.x - misc_x) / misc_w * span
            : editor->view_start;
        double d = snap_to_beat(t_cursor - s_misc_body_grab, beatmap,
                                 editor->view_start, editor->view_end, misc_w)
                   - s_misc_body_t0;
        // Clamp so no member of the group is dragged off the track.
        double lo = 0.0, hi = 0.0;
        bool   any = false;
        for (int i = 0; i < miscmap->count; i++) {
            if (!miscmap->entries[i].selected) continue;
            if (!any || miscmap->entries[i].t_start < lo) lo = miscmap->entries[i].t_start;
            if (!any || miscmap->entries[i].t_end   > hi) hi = miscmap->entries[i].t_end;
            any = true;
        }
        if (any) {
            if (lo + d < 0.0)              d = -lo;
            if (hi + d > editor->duration) d = editor->duration - hi;
        }
        s_misc_body_delta = d;
    }

    // Misc body drag release: commit the move and re-sort
    if (s_misc_body_drag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (fabs(s_misc_body_delta) > 1e-6 && miscmap_selected_count(miscmap) > 0) {
            undo_push(undo, nullptr, nullptr, nullptr, miscmap);
            int focus = s_misc_selected;
            miscmap_move_selection(miscmap, s_misc_body_delta, &focus);
            s_misc_selected = focus;
        }
        s_misc_body_drag  = false;
        s_misc_body_delta = 0.0;
    }

    // Misc handle drag: resize selected annotation
    if (s_misc_hdrag && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (s_misc_hdrag_idx >= 0 && s_misc_hdrag_idx < miscmap->count) {
            double span  = editor->view_end - editor->view_start;
            double t_raw = (misc_w > 0 && span > 0)
                ? editor->view_start + (double)(io.MousePos.x - misc_x) / misc_w * span
                : editor->view_start;
            double t_snap = snap_to_beat(t_raw, beatmap,
                                          editor->view_start, editor->view_end, misc_w);
            MiscAnnotation& ma = miscmap->entries[s_misc_hdrag_idx];
            if (s_misc_hdrag_end == 0)
                ma.t_start = (t_snap < ma.t_end - 0.001) ? t_snap : ma.t_end - 0.001;
            else
                ma.t_end   = (t_snap > ma.t_start + 0.001) ? t_snap : ma.t_start + 0.001;
            miscmap->dirty = true;
        }
    }

    // Misc drag-to-create release
    if (s_misc_drag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        double pt0 = (s_misc_drag_t0 < s_misc_drag_t1) ? s_misc_drag_t0 : s_misc_drag_t1;
        double pt1 = (s_misc_drag_t0 < s_misc_drag_t1) ? s_misc_drag_t1 : s_misc_drag_t0;
        if (pt1 - pt0 > 0.05) {
            undo_push(undo, nullptr, nullptr, nullptr, miscmap);
            int idx = miscmap_add(miscmap, pt0, pt1, "");
            if (idx >= 0) {
                miscmap->entries[idx].selected = true;
                s_misc_selected    = idx;
                s_misc_inline_edit = idx;  // immediately enter text editing
            } else {
                undo_drop_last(undo);
            }
        }
        s_misc_drag = false;
    }

    // Misc handle drag release
    if (s_misc_hdrag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (s_misc_hdrag_idx >= 0 && s_misc_hdrag_idx < miscmap->count) {
            const MiscAnnotation& ma = miscmap->entries[s_misc_hdrag_idx];
            if (fabs(ma.t_start - s_misc_hdrag_t0) < 1e-9 &&
                fabs(ma.t_end   - s_misc_hdrag_t1) < 1e-9)
                undo_drop_last(undo);
        }
        s_misc_hdrag     = false;
        s_misc_hdrag_idx = -1;
    }

    // Misc rubber-band release: everything overlapping the swept x range joins
    // the selection (shift is held, so it adds rather than replaces).
    if (s_misc_rect_sel && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float rx0 = s_misc_rect_x0 < io.MousePos.x ? s_misc_rect_x0 : io.MousePos.x;
        float rx1 = s_misc_rect_x0 < io.MousePos.x ? io.MousePos.x : s_misc_rect_x0;
        for (int i = 0; i < miscmap->count; i++) {
            float mx0 = time_to_x(miscmap->entries[i].t_start,
                                   editor->view_start, editor->view_end, misc_x, misc_w);
            float mx1 = time_to_x(miscmap->entries[i].t_end,
                                   editor->view_start, editor->view_end, misc_x, misc_w);
            if (mx1 >= rx0 && mx0 <= rx1) miscmap->entries[i].selected = true;
        }
        s_misc_rect_sel = false;
    }

    // Delete key: remove the whole misc selection
    if (!ImGui::IsAnyItemActive() &&
            (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        if (s_misc_selected >= 0 && s_misc_selected < miscmap->count)
            miscmap->entries[s_misc_selected].selected = true;
        if (miscmap_selected_count(miscmap) > 0) {
            undo_push(undo, nullptr, nullptr, nullptr, miscmap);
            for (int i = miscmap->count - 1; i >= 0; i--)
                if (miscmap->entries[i].selected) miscmap_remove(miscmap, i);
            s_misc_selected    = -1;
            s_misc_inline_edit = -1;
        }
    }

    // Ctrl/Cmd+C / X / V: copy, cut and paste a group of misc annotations.
    // Paste lands the group's first annotation at the playhead, snapped to the
    // nearest beat, with the rest of the group at their original beat offsets.
    // Holding the playhead still and pasting again tiles the next copy after
    // the last one, so laying a copied bar of chords across eight bars is eight
    // keystrokes rather than eight seek-and-paste round trips.
    if (show_misc && !ImGui::IsAnyItemActive() && s_misc_inline_edit < 0 &&
            (io.KeyCtrl || io.KeySuper)) {
        bool want_copy  = ImGui::IsKeyPressed(ImGuiKey_C);
        bool want_cut   = ImGui::IsKeyPressed(ImGuiKey_X);
        bool want_paste = ImGui::IsKeyPressed(ImGuiKey_V);

        if (want_copy || want_cut) {
            // The focused entry counts as the selection when nothing else is.
            if (s_misc_selected >= 0 && s_misc_selected < miscmap->count)
                miscmap->entries[s_misc_selected].selected = true;
            if (miscmap_selected_count(miscmap) > 0) {
                miscmap_copy_selection(miscmap, beatmap);
                if (want_cut) {
                    undo_push(undo, nullptr, nullptr, nullptr, miscmap);
                    for (int i = miscmap->count - 1; i >= 0; i--)
                        if (miscmap->entries[i].selected) miscmap_remove(miscmap, i);
                    s_misc_selected    = -1;
                    s_misc_inline_edit = -1;
                }
            }
        } else if (want_paste && miscmap_clipboard_count() > 0) {
            double t_anchor = snap_anchor_to_beat(audio_get_position(audio), beatmap);
            undo_push(undo, nullptr, nullptr, nullptr, miscmap);
            if (miscmap_paste_chained(miscmap, beatmap, t_anchor) > 0) {
                s_misc_selected    = -1;   // the pasted group is selected, no focus
                s_misc_inline_edit = -1;
            } else {
                undo_drop_last(undo);
            }
        }
    }

    // Tap rect-select release
    if (s_tap_rect_sel && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float rsx0 = s_tap_rect_x0 < io.MousePos.x ? s_tap_rect_x0 : io.MousePos.x;
        float rsx1 = s_tap_rect_x0 < io.MousePos.x ? io.MousePos.x : s_tap_rect_x0;
        for (int i = 0; i < s_tap_count; i++) {
            float bx = time_to_x(s_taps[i].time,
                                 editor->view_start, editor->view_end, tap_x, tap_w);
            if (bx >= rsx0 && bx <= rsx1)
                s_taps[i].selected = true;
        }
        s_tap_rect_sel = false;
    }

    // Autobeat rect-select release
    if (s_ab_rect_sel && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && autobeat) {
        float rsx0 = s_ab_rect_x0 < io.MousePos.x ? s_ab_rect_x0 : io.MousePos.x;
        float rsx1 = s_ab_rect_x0 < io.MousePos.x ? io.MousePos.x : s_ab_rect_x0;
        for (int i = 0; i < autobeat->beat_count; i++) {
            float bx = time_to_x(autobeat->beat_times[i],
                                 editor->view_start, editor->view_end, ab_x, ab_w);
            if (bx >= rsx0 && bx <= rsx1)
                autobeat->beat_selected[i] = true;
        }
        s_ab_rect_sel = false;
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s_drag_in_spectro  = false;
        s_drag_in_ruler    = false;
        s_mm_seeking       = false;
        s_drag_in_place    = false;
        s_drag_in_tap      = false;
        s_drag_in_autobeat = false;
        s_drag_in_beats    = false;
        s_drag_in_sec      = false;
        s_drag_in_lyr      = false;
        s_drag_in_misc     = false;
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

    // T key: record a tap at the current playhead position (play mode only)
    if (audio->playing && !ImGui::IsAnyItemActive() &&
            ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        double t = audio_get_position(audio);
        if (s_tap_count < MAX_TAPS)
            s_taps[s_tap_count++] = { t, false };
    }

    // I key: insert selected taps into the beatmap as real beats
    if (!ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_I, false)) {
        bool any = false;
        for (int i = 0; i < s_tap_count; i++)
            if (s_taps[i].selected) { any = true; break; }
        if (any) {
            undo_push(undo, beatmap, lyricmap);
            for (int i = 0; i < s_tap_count; i++)
                if (s_taps[i].selected)
                    beatmap_add(beatmap, s_taps[i].time);
            int j = 0;
            for (int i = 0; i < s_tap_count; i++)
                if (!s_taps[i].selected)
                    s_taps[j++] = s_taps[i];
            s_tap_count = j;
        }
    }

    // Delete/Backspace: remove selected taps
    if (!ImGui::IsAnyItemActive() &&
            (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        int j = 0;
        for (int i = 0; i < s_tap_count; i++)
            if (!s_taps[i].selected)
                s_taps[j++] = s_taps[i];
        s_tap_count = j;
    }

    // I key: also inserts selected auto-beats into the beatmap
    if (!ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_I, false) && autobeat) {
        bool any = false;
        for (int i = 0; i < autobeat->beat_count; i++)
            if (autobeat->beat_selected[i]) { any = true; break; }
        if (any) {
            undo_push(undo, beatmap, lyricmap);
            for (int i = 0; i < autobeat->beat_count; i++)
                if (autobeat->beat_selected[i])
                    beatmap_add(beatmap, autobeat->beat_times[i]);
            int j = 0;
            for (int i = 0; i < autobeat->beat_count; i++)
                if (!autobeat->beat_selected[i]) {
                    autobeat->beat_times[j]    = autobeat->beat_times[i];
                    autobeat->beat_selected[j] = false;
                    j++;
                }
            autobeat->beat_count = j;
        }
    }

    // --- Draw ---

    // Left sidebar: background + right border
    dl->AddRectFilled(ImVec2(rx, ry), ImVec2(cx, ry + total_h),
                      IM_COL32(12, 12, 18, 255));
    dl->AddLine(ImVec2(cx, ry), ImVec2(cx, ry + total_h),
                IM_COL32(50, 50, 70, 255));

    // Beat-editor collapse triangle in the left sidebar (owned by the insert row)
    if (show_place) {
        float tcx = sb_x + sb_w * 0.5f;
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

    // Spectrogram controls in left sidebar: Log toggle, then +/- max-freq buttons
    {
        bool at_max = (s_spectro_max_khz >= 22);
        bool at_min = (s_spectro_max_khz <= 2);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 3.0f));
        float bw  = sb_w - 4.0f;
        float bh  = ImGui::GetFrameHeight();
        float bx  = sb_x + 2.0f;
        float by  = ty + 2.0f;

        // Log toggle button — snapshot state before Button() which may flip it
        bool log_active = s_spectro_log;
        if (log_active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(45, 100, 55, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 130, 70, 255));
        }
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        if (ImGui::Button("Log##sl", ImVec2(bw, 0))) s_spectro_log = !s_spectro_log;
        if (log_active) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Logarithmic frequency axis");
        by += bh + 2.0f;

        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        if (at_max) ImGui::BeginDisabled();
        if (ImGui::Button("+##mfp", ImVec2(bw, 0))) s_spectro_max_khz++;
        bool ph = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (at_max) ImGui::EndDisabled();
        if (ph) ImGui::SetTooltip("Max freq +1 kHz (now %d kHz)", s_spectro_max_khz);

        ImGui::SetCursorScreenPos(ImVec2(bx, by + bh + 2.0f));
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
        float log_range = s_spectro_log ? logf(max_freq_hz / SPECTRO_LOG_FMIN) : 0.0f;
        for (int i = n_sb - 1; i >= 0; i--) {  // high-freq → low-freq  (top → bottom)
            if (sb_freqs[i] >= (int)max_freq_hz) continue;
            float frac;
            if (s_spectro_log && log_range > 0.0f && sb_freqs[i] > SPECTRO_LOG_FMIN)
                frac = 1.0f - logf((float)sb_freqs[i] / SPECTRO_LOG_FMIN) / log_range;
            else
                frac = 1.0f - (float)sb_freqs[i] / max_freq_hz;
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
                       (float)(s_spectro_max_khz * 1000), s_spectro_log);

    // Chroma hover overlay: faint green band for each octave of the hovered note
    if (editor->chroma_hover_note >= 0 && spectro->computed && spectro->sample_rate > 0) {
        float nyquist     = (float)(spectro->sample_rate / 2);
        float max_freq_hz = (float)(s_spectro_max_khz * 1000);
        if (max_freq_hz > nyquist) max_freq_hz = nyquist;

        int pc = editor->chroma_hover_note;  // 0=C .. 11=B
        for (int oct = 2; oct <= 6; oct++) {
            int   midi         = 12 * (oct + 1) + pc;
            float freq_center  = 440.0f * powf(2.0f, (midi - 69) / 12.0f);
            float freq_lo      = freq_center * 0.9717f;  // -50 cents
            float freq_hi      = freq_center * 1.0293f;  // +50 cents
            if (freq_lo >= max_freq_hz) continue;        // above visible range
            if (freq_hi <= 0.0f)        continue;
            if (freq_lo < 0.0f) freq_lo = 0.0f;
            if (freq_hi > max_freq_hz) freq_hi = max_freq_hz;

            // Frequency → y: higher freq = smaller y (higher on screen)
            float y_top, y_bot;
            if (s_spectro_log) {
                float lr = logf(max_freq_hz / SPECTRO_LOG_FMIN);
                float fhi = freq_hi > SPECTRO_LOG_FMIN ? freq_hi : SPECTRO_LOG_FMIN;
                float flo = freq_lo > SPECTRO_LOG_FMIN ? freq_lo : SPECTRO_LOG_FMIN;
                y_top = ty + th * (1.0f - logf(fhi / SPECTRO_LOG_FMIN) / lr);
                y_bot = ty + th * (1.0f - logf(flo / SPECTRO_LOG_FMIN) / lr);
            } else {
                y_top = ty + th * (1.0f - freq_hi / max_freq_hz);
                y_bot = ty + th * (1.0f - freq_lo / max_freq_hz);
            }
            if (y_top < ty)        y_top = ty;
            if (y_bot > ty + th)   y_bot = ty + th;
            if (y_bot <= y_top + 0.5f) y_bot = y_top + 1.0f;

            dl->AddRectFilled(ImVec2(tx, y_top), ImVec2(tx + tw, y_bot),
                              IM_COL32(100, 255, 120, 50));
        }
    }

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
    if (show_place) {
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
        if (io.MousePos.x >= sb_x && io.MousePos.x < cx && io.MousePos.y >= ps_y && io.MousePos.y < ps_y + PLACE_STRIP_H)
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
    }  // end show_place_strip

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

    // --- Tap strip ---
    if (show_taps) {
    dl->AddRectFilled(ImVec2(tap_x, tap_y), ImVec2(tap_x + tap_w, tap_y + TAP_STRIP_H),
                      IM_COL32(10, 18, 14, 255));
    dl->AddRect(ImVec2(tap_x, tap_y), ImVec2(tap_x + tap_w, tap_y + TAP_STRIP_H),
                IM_COL32(40, 65, 50, 255));
    dl->AddText(ImVec2(cx + 4.0f, tap_y + 3.0f), IM_COL32(90, 110, 90, 100), "Taps");

    {
        const float TAP_R  = 5.0f;
        float strip_cy = tap_y + TAP_STRIP_H * 0.5f;
        double span    = editor->view_end - editor->view_start;
        // Hover hit-test (find nearest tap to mouse, within the strip y-band)
        int   tap_hover      = -1;
        float tap_hover_best = 9.0f;
        bool  in_tap_y = (io.MousePos.y >= tap_y && io.MousePos.y < tap_y + TAP_STRIP_H);
        if (in_tap_y) {
            for (int i = 0; i < s_tap_count; i++) {
                float bx = (span > 0.0 && tap_w > 0)
                    ? tap_x + (float)((s_taps[i].time - editor->view_start) / span * tap_w)
                    : tap_x;
                float dx = fabsf(io.MousePos.x - bx);
                if (dx < tap_hover_best) { tap_hover_best = dx; tap_hover = i; }
            }
        }
        dl->PushClipRect(ImVec2(tap_x, tap_y), ImVec2(tap_x + tap_w, tap_y + TAP_STRIP_H), true);
        for (int i = 0; i < s_tap_count; i++) {
            float bx = (span > 0.0 && tap_w > 0)
                ? tap_x + (float)((s_taps[i].time - editor->view_start) / span * tap_w)
                : tap_x;
            bool  hov    = (i == tap_hover);
            float r      = hov ? TAP_R + 2.0f : TAP_R;
            if (bx < tap_x - r || bx > tap_x + tap_w + r) continue;
            bool  sel    = s_taps[i].selected;
            ImU32 fill   = hov  ? IM_COL32(200, 240, 255, 255)
                         : sel  ? IM_COL32(100, 200, 255, 220)
                                : IM_COL32(120, 200, 140, 190);
            ImU32 border = hov  ? IM_COL32(255, 255, 255, 255)
                         : sel  ? IM_COL32(160, 230, 255, 255)
                                : IM_COL32(160, 230, 160, 220);
            draw_diamond(dl, bx, strip_cy, r, fill, border);
        }
        // Rect-select highlight
        if (s_tap_rect_sel && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float rsx0 = s_tap_rect_x0 < io.MousePos.x ? s_tap_rect_x0 : io.MousePos.x;
            float rsx1 = s_tap_rect_x0 < io.MousePos.x ? io.MousePos.x : s_tap_rect_x0;
            float cl0 = rsx0 < tap_x ? tap_x : rsx0;
            float cl1 = rsx1 > tap_x + tap_w ? tap_x + tap_w : rsx1;
            if (cl1 > cl0)
                dl->AddRectFilled(ImVec2(cl0, tap_y), ImVec2(cl1, tap_y + TAP_STRIP_H),
                                  IM_COL32(100, 200, 255, 40));
        }
        // Instantaneous BPM as data labels between taps
        if (editor->show_bpm_labels && s_tap_count >= 2) {
            static double s_tap_times[MAX_TAPS];
            for (int i = 0; i < s_tap_count; i++) s_tap_times[i] = s_taps[i].time;
            TimeSeq seq = { nullptr, s_tap_times, s_tap_count };
            draw_bpm_labels(dl, seq, tap_x, tap_y, tap_w, TAP_STRIP_H,
                            editor->view_start, editor->view_end,
                            false, 0.0f, 0.0f, TAP_R,
                            IM_COL32(160, 230, 160, 200));
        }
        dl->PopClipRect();
        // Hover BPM labels: instantaneous tempo to the left/right of the hovered tap
        if (tap_hover >= 0 && in_tap_y) {
            float bx = time_to_x(s_taps[tap_hover].time,
                                  editor->view_start, editor->view_end, tap_x, tap_w);
            float r = TAP_R + 2.0f;
            float ly = bpm_label_y(strip_cy, r, ImGui::GetTextLineHeight(),
                                   tap_y, tap_y + TAP_STRIP_H);
            char  buf[32];
            ImVec2 ts;
            if (tap_hover > 0) {
                double dt = s_taps[tap_hover].time - s_taps[tap_hover - 1].time;
                if (dt > 1e-6) {
                    snprintf(buf, sizeof(buf), "%.1f", 60.0 / dt);
                    ts = ImGui::CalcTextSize(buf);
                    float lx = bx - r - 4.0f - ts.x;
                    if (lx >= tap_x)
                        dl->AddText(ImVec2(lx, ly),
                                    IM_COL32(160, 230, 160, 220), buf);
                }
            }
            if (tap_hover < s_tap_count - 1) {
                double dt = s_taps[tap_hover + 1].time - s_taps[tap_hover].time;
                if (dt > 1e-6) {
                    snprintf(buf, sizeof(buf), "%.1f", 60.0 / dt);
                    ts = ImGui::CalcTextSize(buf);
                    float lx = bx + r + 4.0f;
                    if (lx + ts.x <= tap_x + tap_w)
                        dl->AddText(ImVec2(lx, ly),
                                    IM_COL32(160, 230, 160, 220), buf);
                }
            }
        }
    }
    }  // end show_tap_strip

    // --- Auto-beat strip ---
    if (show_auto && autobeat) {
    dl->AddRectFilled(ImVec2(ab_x, ab_y), ImVec2(ab_x + ab_w, ab_y + AUTOBEAT_STRIP_H),
                      IM_COL32(20, 10, 10, 255));
    dl->AddRect(ImVec2(ab_x, ab_y), ImVec2(ab_x + ab_w, ab_y + AUTOBEAT_STRIP_H),
                IM_COL32(80, 30, 30, 255));
    dl->AddText(ImVec2(cx + 4.0f, ab_y + 3.0f), IM_COL32(130, 60, 60, 100), "Auto");

    {
        float  strip_cy = ab_y + AUTOBEAT_STRIP_H * 0.5f;
        // Hover hit-test
        int   ab_hover      = -1;
        float ab_hover_best = 9.0f;
        bool  in_ab_y = (io.MousePos.y >= ab_y && io.MousePos.y < ab_y + AUTOBEAT_STRIP_H);
        if (in_ab_y) {
            for (int i = 0; i < autobeat->beat_count; i++) {
                float bx = time_to_x(autobeat->beat_times[i],
                                     editor->view_start, editor->view_end, ab_x, ab_w);
                float dx = fabsf(io.MousePos.x - bx);
                if (dx < ab_hover_best) { ab_hover_best = dx; ab_hover = i; }
            }
        }
        dl->PushClipRect(ImVec2(ab_x, ab_y), ImVec2(ab_x + ab_w, ab_y + AUTOBEAT_STRIP_H), true);

        // Raw onset ticks (subtle vertical lines)
        if (editor->show_raw_onsets) {
            for (int i = 0; i < autobeat->onset_count; i++) {
                float bx = time_to_x(autobeat->onset_times[i],
                                     editor->view_start, editor->view_end, ab_x, ab_w);
                if (bx < ab_x || bx > ab_x + ab_w) continue;
                dl->AddLine(ImVec2(bx, ab_y + 2.0f),
                            ImVec2(bx, ab_y + AUTOBEAT_STRIP_H - 2.0f),
                            IM_COL32(200, 80, 80, 120), 1.0f);
            }
        }

        // Beat diamonds
        const float AB_R = 5.0f;
        for (int i = 0; i < autobeat->beat_count; i++) {
            float bx = time_to_x(autobeat->beat_times[i],
                                 editor->view_start, editor->view_end, ab_x, ab_w);
            bool  hov    = (i == ab_hover);
            float r      = hov ? AB_R + 2.0f : AB_R;
            if (bx < ab_x - r || bx > ab_x + ab_w + r) continue;
            bool  sel    = autobeat->beat_selected[i];
            ImU32 fill   = hov  ? IM_COL32(255, 200, 200, 255)
                         : sel  ? IM_COL32(220,  50,  50, 230)
                                : IM_COL32(140,  35,  35, 180);
            ImU32 border = hov  ? IM_COL32(255, 230, 230, 255)
                         : sel  ? IM_COL32(255, 120, 120, 255)
                                : IM_COL32(200,  80,  80, 200);
            draw_diamond(dl, bx, strip_cy, r, fill, border);
        }

        // Rect-select highlight
        if (s_ab_rect_sel && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float rsx0 = s_ab_rect_x0 < io.MousePos.x ? s_ab_rect_x0 : io.MousePos.x;
            float rsx1 = s_ab_rect_x0 < io.MousePos.x ? io.MousePos.x : s_ab_rect_x0;
            float cl0  = rsx0 < ab_x           ? ab_x           : rsx0;
            float cl1  = rsx1 > ab_x + ab_w    ? ab_x + ab_w    : rsx1;
            if (cl1 > cl0)
                dl->AddRectFilled(ImVec2(cl0, ab_y), ImVec2(cl1, ab_y + AUTOBEAT_STRIP_H),
                                  IM_COL32(220, 60, 60, 35));
        }
        // Instantaneous BPM as data labels between auto-beats
        if (editor->show_bpm_labels && autobeat->beat_count >= 2) {
            TimeSeq seq = { nullptr, autobeat->beat_times, autobeat->beat_count };
            draw_bpm_labels(dl, seq, ab_x, ab_y, ab_w, AUTOBEAT_STRIP_H,
                            editor->view_start, editor->view_end,
                            false, 0.0f, 0.0f, AB_R,
                            IM_COL32(255, 150, 150, 200));
        }
        dl->PopClipRect();
        // Hover BPM labels: instantaneous tempo to the left/right of the hovered auto-beat
        if (ab_hover >= 0 && in_ab_y) {
            float bx = time_to_x(autobeat->beat_times[ab_hover],
                                  editor->view_start, editor->view_end, ab_x, ab_w);
            float r = AB_R + 2.0f;
            float ly = bpm_label_y(strip_cy, r, ImGui::GetTextLineHeight(),
                                   ab_y, ab_y + AUTOBEAT_STRIP_H);
            char  buf[32];
            ImVec2 ts;
            if (ab_hover > 0) {
                double dt = autobeat->beat_times[ab_hover] - autobeat->beat_times[ab_hover - 1];
                if (dt > 1e-6) {
                    snprintf(buf, sizeof(buf), "%.1f", 60.0 / dt);
                    ts = ImGui::CalcTextSize(buf);
                    float lx = bx - r - 4.0f - ts.x;
                    if (lx >= ab_x)
                        dl->AddText(ImVec2(lx, ly),
                                    IM_COL32(255, 150, 150, 220), buf);
                }
            }
            if (ab_hover < autobeat->beat_count - 1) {
                double dt = autobeat->beat_times[ab_hover + 1] - autobeat->beat_times[ab_hover];
                if (dt > 1e-6) {
                    snprintf(buf, sizeof(buf), "%.1f", 60.0 / dt);
                    ts = ImGui::CalcTextSize(buf);
                    float lx = bx + r + 4.0f;
                    if (lx + ts.x <= ab_x + ab_w)
                        dl->AddText(ImVec2(lx, ly),
                                    IM_COL32(255, 150, 150, 220), buf);
                }
            }
        }
    }
    }  // end show_autobeat_strip

    // Beat area background
    if (show_beats) {
    dl->AddRectFilled(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h),
                      IM_COL32(14, 14, 22, 255));
    dl->AddRect(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h),
                IM_COL32(50, 50, 70, 255));
    // Tempo graph behind the beat markers
    if (show_tempo && !s_beats_collapsed && ba_h > 8.0f && beatmap->count >= 2) {
        float lo = editor->tempo_min_bpm, hi = editor->tempo_max_bpm;
        if (hi <= lo) hi = lo + 1.0f;

        dl->PushClipRect(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h), true);
        draw_tempo_scale(dl, ba_x, ba_y, ba_w, ba_h, lo, hi);

        TimeSeq seq = { beatmap->beats, nullptr, beatmap->count };
        draw_tempo_ticks(dl, seq, ba_x, ba_y, ba_w, ba_h,
                         editor->view_start, editor->view_end, lo, hi,
                         IM_COL32(255, 205,  70, 130),   // in range
                         IM_COL32(220, 110,  60, 130),   // clamped to an edge
                         1.5f);
        draw_tempo_average(dl, seq, ba_x, ba_y, ba_w, ba_h,
                           editor->view_start, editor->view_end, lo, hi,
                           editor->tempo_avg_window,
                           IM_COL32(190, 135,  20, 230), 2.0f);

        // Smoothing preview: proposed tempo in the same graph
        const SmoothPreview* pv = ui_smoothing_preview();
        if (pv->active && pv->n >= 2) {
            TimeSeq pseq = { nullptr, pv->times, pv->n };
            draw_tempo_ticks(dl, pseq, ba_x, ba_y, ba_w, ba_h,
                             editor->view_start, editor->view_end, lo, hi,
                             IM_COL32(120, 255, 170, 130), IM_COL32(120, 255, 170, 70), 1.5f);
            draw_tempo_average(dl, pseq, ba_x, ba_y, ba_w, ba_h,
                               editor->view_start, editor->view_end, lo, hi,
                               editor->tempo_avg_window,
                               IM_COL32( 80, 220, 130, 230), 2.0f);
        }
        dl->PopClipRect();
    }

    // Instantaneous BPM as data labels on every interval
    if (editor->show_bpm_labels && !s_beats_collapsed && ba_h > 8.0f &&
        beatmap->count >= 2) {
        float lo = editor->tempo_min_bpm, hi = editor->tempo_max_bpm;
        if (hi <= lo) hi = lo + 1.0f;
        TimeSeq seq = { beatmap->beats, nullptr, beatmap->count };
        dl->PushClipRect(ImVec2(ba_x, ba_y), ImVec2(ba_x + ba_w, ba_y + ba_h), true);
        draw_bpm_labels(dl, seq, ba_x, ba_y, ba_w, ba_h,
                        editor->view_start, editor->view_end,
                        show_tempo, lo, hi, DIAMOND_R,
                        IM_COL32(220, 220, 140, 200));
        dl->PopClipRect();
    }

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

    // Smoothing preview: where each beat would land, and how far it moves.
    {
        const SmoothPreview* pv = ui_smoothing_preview();
        if (pv->active && pv->n > 0 && ba_h > 0.0f) {
            const ImU32 GHOST = IM_COL32(120, 255, 170, 190);
            for (int k = 0; k < pv->n; k++) {
                int idx = pv->i0 + k;
                if (idx < 0 || idx >= beatmap->count) break;
                double t_old = beatmap->beats[idx].time;
                double t_new = pv->times[k];
                float  x_old = time_to_x(t_old, editor->view_start, editor->view_end, ba_x, ba_w);
                float  x_new = time_to_x(t_new, editor->view_start, editor->view_end, ba_x, ba_w);
                if ((x_old < ba_x - 8.0f && x_new < ba_x - 8.0f) ||
                    (x_old > ba_x + ba_w + 8.0f && x_new > ba_x + ba_w + 8.0f)) continue;
                dl->AddLine(ImVec2(x_new, ba_y + 2.0f), ImVec2(x_new, ba_y + ba_h - 2.0f),
                            GHOST, 1.0f);
                // Connector, only when the move is actually visible at this zoom
                if (fabsf(x_new - x_old) >= 1.5f) {
                    float cy_mid = ba_y + ba_h * 0.5f;
                    dl->AddLine(ImVec2(x_old, cy_mid), ImVec2(x_new, cy_mid), GHOST, 1.0f);
                }
            }
        }
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
                float ly   = bpm_label_y(cy, r, ImGui::GetTextLineHeight(),
                                         ba_y, ba_y + ba_h);
                char  buf[32];
                ImVec2 ts;

                if (idx > 0) {
                    double dt = beatmap->beats[idx].time - beatmap->beats[idx - 1].time;
                    if (dt > 1e-6) {
                        snprintf(buf, sizeof(buf), "%.1f", 60.0 / dt);
                        ts = ImGui::CalcTextSize(buf);
                        float lx = bx - r - 4.0f - ts.x;
                        if (lx >= ba_x)
                            dl->AddText(ImVec2(lx, ly),
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
                            dl->AddText(ImVec2(lx, ly),
                                        IM_COL32(220, 220, 120, 220), buf);
                    }
                }
            }
        }
    }
    }  // end show_beat_strip

    // --- Section strip ---
    if (show_sect) {
    dl->AddRectFilled(ImVec2(sa_x, sa_y), ImVec2(sa_x + sa_w, sa_y + sa_h),
                      IM_COL32(18, 18, 26, 255));
    dl->AddRect(ImVec2(sa_x, sa_y), ImVec2(sa_x + sa_w, sa_y + sa_h),
                IM_COL32(60, 60, 85, 255));
    dl->AddText(ImVec2(cx + 4.0f, sa_y + 3.0f), IM_COL32(110, 110, 140, 160), "Sections");

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
    }  // end show_section_strip

    // --- Lyric strip ---
    if (show_lyr) {
    dl->AddRectFilled(ImVec2(la_x, la_y), ImVec2(la_x + la_w, la_y + la_h),
                      IM_COL32(18, 22, 28, 255));
    dl->AddRect(ImVec2(la_x, la_y), ImVec2(la_x + la_w, la_y + la_h),
                IM_COL32(55, 60, 80, 255));
    dl->AddText(ImVec2(cx + 4.0f, la_y + 3.0f), IM_COL32(110, 110, 140, 160), "Lyrics");

    dl->PushClipRect(ImVec2(la_x, la_y), ImVec2(la_x + la_w, la_y + la_h), true);
    for (int i = 0; i < lyricmap->count; i++) {
        const Lyric& ly = lyricmap->lyrics[i];
        // Use virtual position while this lyric is being body-dragged
        bool   bdrag = (s_lyr_body_drag && s_lyr_body_drag_idx == i);
        double vis_t0 = bdrag ? s_lyr_body_new_t0                        : ly.t_start;
        double vis_t1 = bdrag ? s_lyr_body_new_t0 + s_lyr_body_drag_dur : ly.t_end;
        float lx0 = time_to_x(vis_t0, editor->view_start, editor->view_end, la_x, la_w);
        float lx1 = time_to_x(vis_t1, editor->view_start, editor->view_end, la_x, la_w);
        if (lx1 <= la_x || lx0 >= la_x + la_w) continue;
        bool  sel = (i == s_lyr_selected);
        float y0  = la_y + 3.0f, y1 = la_y + la_h - 3.0f;

        dl->AddRectFilled(ImVec2(lx0, y0), ImVec2(lx1, y1),
                          bdrag ? IM_COL32(70, 150, 200, 200) : IM_COL32(45, 95, 130, 160));
        dl->AddRect(ImVec2(lx0, y0), ImVec2(lx1, y1),
                    sel ? IM_COL32(90, 180, 220, 230) : IM_COL32(0, 0, 0, 120),
                    0.0f, 0, sel ? 2.0f : 1.0f);

        // Lyric text, left-aligned inside the rect
        if (ly.text[0]) {
            ImFont* lf      = s_lyric_font();
            float   lf_sz   = lf->FontSize;
            float   th_lbl  = lf_sz;
            float   lbl_y   = y0 + (y1 - y0 - th_lbl) * 0.5f;
            float   lbl_x   = lx0 + 4.0f;
            ImVec2  ts      = lf->CalcTextSizeA(lf_sz, FLT_MAX, 0.0f, ly.text);
            // Clip text to lyric rect width
            if (lbl_x + ts.x < lx1 - 2.0f)
                dl->AddText(lf, lf_sz, ImVec2(lbl_x, lbl_y), IM_COL32(220, 220, 200, 230), ly.text);
            else if (lx1 - lx0 > 10.0f) {
                // Ellipsis: push clip rect for the individual lyric
                dl->PushClipRect(ImVec2(lx0, y0), ImVec2(lx1 - 2.0f, y1), true);
                dl->AddText(lf, lf_sz, ImVec2(lbl_x, lbl_y), IM_COL32(220, 220, 200, 230), ly.text);
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

    // --- Lyric Index toggle button (right end of lyric strip) ---
    {
        const float BTN_W = 26.0f;
        const float BTN_H = LYRIC_H - 4.0f;
        float bx = la_x + la_w - BTN_W - 2.0f;
        float by = la_y + 2.0f;

        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Button,
            s_lyric_index_open ? IM_COL32(55, 95, 160, 210) : IM_COL32(28, 28, 50, 180));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 120, 190, 230));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(90, 150, 220, 255));
        if (ImGui::Button("##lyridx_tog", ImVec2(BTN_W, BTN_H)))
            s_lyric_index_open = !s_lyric_index_open;
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lyric Index");

        // Draw icon: ≡ when closed, × when open
        float icx = bx + BTN_W * 0.5f;
        float icy = by + BTN_H * 0.5f;
        ImU32 ico = s_lyric_index_open ? IM_COL32(220, 190, 120, 230)
                                       : IM_COL32(160, 160, 195, 220);
        if (s_lyric_index_open) {
            dl->AddLine(ImVec2(icx - 5.0f, icy - 5.0f), ImVec2(icx + 5.0f, icy + 5.0f), ico, 1.5f);
            dl->AddLine(ImVec2(icx + 5.0f, icy - 5.0f), ImVec2(icx - 5.0f, icy + 5.0f), ico, 1.5f);
        } else {
            dl->AddLine(ImVec2(icx - 6.0f, icy - 3.5f), ImVec2(icx + 6.0f, icy - 3.5f), ico, 1.5f);
            dl->AddLine(ImVec2(icx - 6.0f, icy),        ImVec2(icx + 6.0f, icy),        ico, 1.5f);
            dl->AddLine(ImVec2(icx - 6.0f, icy + 3.5f), ImVec2(icx + 6.0f, icy + 3.5f), ico, 1.5f);
        }
    }
    }  // end show_lyric_strip

    // --- Inline lyric edit (InputText overlay on the lyric block) ---
    {
        static int  s_lyr_ie_prev_idx   = -1;
        static bool s_lyr_ie_was_active = false;

        if (s_lyr_inline_edit >= 0 && s_lyr_inline_edit < lyricmap->count
                && show_lyr) {
            bool is_new = (s_lyr_inline_edit != s_lyr_ie_prev_idx);
            s_lyr_ie_prev_idx = s_lyr_inline_edit;

            Lyric& ly = lyricmap->lyrics[s_lyr_inline_edit];
            float lx0 = time_to_x(ly.t_start, editor->view_start, editor->view_end, la_x, la_w);
            float lx1 = time_to_x(ly.t_end,   editor->view_start, editor->view_end, la_x, la_w);
            float y0  = la_y + 3.0f, y1 = la_y + la_h - 3.0f;
            float iw  = lx1 - lx0;
            if (iw < 60.0f) iw = 60.0f;

            ImGui::SetCursorScreenPos(ImVec2(lx0, y0));
            ImGui::SetNextItemWidth(iw);
            ImGui::PushFont(s_lyric_font());
            float lf_sz = s_lyric_font()->FontSize;
            float pad_y = (y1 - y0 - lf_sz) * 0.5f;
            if (pad_y < 1.0f) pad_y = 1.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, pad_y));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(20, 55, 85, 230));
            if (is_new) ImGui::SetKeyboardFocusHere(0);
            bool submitted = ImGui::InputText("##lyr_ie", ly.text, sizeof(ly.text),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::PopFont();
            bool cur_active = ImGui::IsItemActive();
            if (cur_active) lyricmap->dirty = true;
            if (submitted || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                s_lyr_inline_edit = -1;
            else if (!cur_active && s_lyr_ie_was_active)
                s_lyr_inline_edit = -1;  // clicked away
            s_lyr_ie_was_active = cur_active;
        } else {
            s_lyr_ie_prev_idx   = -1;
            s_lyr_ie_was_active = false;
        }
    }

    // --- Misc annotations strip ---
    if (show_misc) {
        dl->AddRectFilled(ImVec2(misc_x, misc_y),
                          ImVec2(misc_x + misc_w, misc_y + MISC_STRIP_H),
                          IM_COL32(20, 18, 28, 255));
        dl->AddRect(ImVec2(misc_x, misc_y),
                    ImVec2(misc_x + misc_w, misc_y + MISC_STRIP_H),
                    IM_COL32(60, 55, 80, 255));
        // Header carries the selection and clipboard counts -- otherwise a
        // group cut is invisible until something is pasted.
        char misc_hdr[64] = "Misc";
        {
            int nsel = miscmap_selected_count(miscmap);
            int nclip = miscmap_clipboard_count();
            if (nsel > 1 && nclip > 0)
                snprintf(misc_hdr, sizeof(misc_hdr), "Misc  %d sel  %d copied", nsel, nclip);
            else if (nsel > 1)
                snprintf(misc_hdr, sizeof(misc_hdr), "Misc  %d sel", nsel);
            else if (nclip > 0)
                snprintf(misc_hdr, sizeof(misc_hdr), "Misc  %d copied", nclip);
        }
        dl->AddText(ImVec2(cx + 4.0f, misc_y + 3.0f),
                    IM_COL32(120, 110, 150, 160), misc_hdr);

        dl->PushClipRect(ImVec2(misc_x, misc_y),
                         ImVec2(misc_x + misc_w, misc_y + MISC_STRIP_H), true);
        const float MISC_PAD = 3.0f;
        float my0 = misc_y + MISC_PAD, my1 = misc_y + MISC_STRIP_H - MISC_PAD;
        for (int i = 0; i < miscmap->count; i++) {
            const MiscAnnotation& ma = miscmap->entries[i];
            // A body drag is only virtual until release, so draw the moving
            // group at its dragged position.
            double vd = (s_misc_body_drag && ma.selected) ? s_misc_body_delta : 0.0;
            float mx0 = time_to_x(ma.t_start + vd, editor->view_start, editor->view_end,
                                   misc_x, misc_w);
            float mx1 = time_to_x(ma.t_end   + vd, editor->view_start, editor->view_end,
                                   misc_x, misc_w);
            if (mx1 <= misc_x || mx0 >= misc_x + misc_w) continue;
            bool sel   = ma.selected || (i == s_misc_selected);
            bool focus = (i == s_misc_selected);
            dl->AddRectFilled(ImVec2(mx0, my0), ImVec2(mx1, my1),
                              sel ? IM_COL32(90, 55, 130, 180) : IM_COL32(65, 40, 95, 140));
            dl->AddRect(ImVec2(mx0, my0), ImVec2(mx1, my1),
                        sel ? IM_COL32(160, 120, 210, 230) : IM_COL32(0, 0, 0, 100),
                        0.0f, 0, sel ? 2.0f : 1.0f);
            if (ma.text[0] && mx1 - mx0 > 8.0f) {
                float lbl_y = my0 + (my1 - my0 - ImGui::GetTextLineHeight()) * 0.5f;
                ImVec2 ts = ImGui::CalcTextSize(ma.text);
                if (mx0 + 4.0f + ts.x < mx1 - 2.0f) {
                    dl->AddText(ImVec2(mx0 + 4.0f, lbl_y),
                                IM_COL32(220, 210, 240, 230), ma.text);
                } else {
                    dl->PushClipRect(ImVec2(mx0, my0), ImVec2(mx1 - 2.0f, my1), true);
                    dl->AddText(ImVec2(mx0 + 4.0f, lbl_y),
                                IM_COL32(220, 210, 240, 230), ma.text);
                    dl->PopClipRect();
                }
            }
            // Resize handles for the focused annotation only -- an edge drag
            // acts on one entry, so the rest of the group must not offer them.
            if (focus) {
                float hmy = my0 + (my1 - my0) * 0.5f;
                dl->AddRectFilled(ImVec2(mx0 - 3.0f, hmy - 7.0f),
                                  ImVec2(mx0 + 3.0f, hmy + 7.0f),
                                  IM_COL32(160, 120, 210, 230));
                dl->AddRectFilled(ImVec2(mx1 - 3.0f, hmy - 7.0f),
                                  ImVec2(mx1 + 3.0f, hmy + 7.0f),
                                  IM_COL32(160, 120, 210, 230));
            }
        }
        // Drag-to-create preview
        if (s_misc_drag) {
            double dt0 = (s_misc_drag_t0 < s_misc_drag_t1) ? s_misc_drag_t0 : s_misc_drag_t1;
            double dt1 = (s_misc_drag_t0 < s_misc_drag_t1) ? s_misc_drag_t1 : s_misc_drag_t0;
            float  dx0 = time_to_x(dt0, editor->view_start, editor->view_end, misc_x, misc_w);
            float  dx1 = time_to_x(dt1, editor->view_start, editor->view_end, misc_x, misc_w);
            if (dx1 > dx0) {
                dl->AddRectFilled(ImVec2(dx0, my0), ImVec2(dx1, my1),
                                  IM_COL32(130, 80, 180, 60));
                dl->AddRect(ImVec2(dx0, my0), ImVec2(dx1, my1),
                            IM_COL32(170, 120, 220, 200), 0.0f, 0, 1.5f);
            }
        }
        // Rubber-band select preview
        if (s_misc_rect_sel) {
            float rx0 = s_misc_rect_x0 < io.MousePos.x ? s_misc_rect_x0 : io.MousePos.x;
            float rx1 = s_misc_rect_x0 < io.MousePos.x ? io.MousePos.x : s_misc_rect_x0;
            dl->AddRectFilled(ImVec2(rx0, misc_y), ImVec2(rx1, misc_y + MISC_STRIP_H),
                              IM_COL32(160, 140, 220, 40));
            dl->AddRect(ImVec2(rx0, misc_y), ImVec2(rx1, misc_y + MISC_STRIP_H),
                        IM_COL32(180, 160, 240, 160));
        }
        dl->PopClipRect();
    }  // end show_misc_strip

    // --- Inline misc edit (InputText overlay on the annotation block) ---
    {
        static int  s_misc_ie_prev_idx   = -1;
        static bool s_misc_ie_was_active = false;

        if (s_misc_inline_edit >= 0 && s_misc_inline_edit < miscmap->count
                && show_misc) {
            bool is_new = (s_misc_inline_edit != s_misc_ie_prev_idx);
            s_misc_ie_prev_idx = s_misc_inline_edit;

            MiscAnnotation& ma = miscmap->entries[s_misc_inline_edit];
            float mx0 = time_to_x(ma.t_start, editor->view_start, editor->view_end,
                                   misc_x, misc_w);
            float mx1 = time_to_x(ma.t_end,   editor->view_start, editor->view_end,
                                   misc_x, misc_w);
            float my0 = misc_y + 3.0f, my1 = misc_y + MISC_STRIP_H - 3.0f;
            float iw  = mx1 - mx0;
            if (iw < 60.0f) iw = 60.0f;

            ImGui::SetCursorScreenPos(ImVec2(mx0, my0));
            ImGui::SetNextItemWidth(iw);
            float pad_y = (my1 - my0 - ImGui::GetTextLineHeight()) * 0.5f;
            if (pad_y < 1.0f) pad_y = 1.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, pad_y));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(50, 25, 75, 230));
            if (is_new) ImGui::SetKeyboardFocusHere(0);
            bool submitted = ImGui::InputText("##misc_ie", ma.text, sizeof(ma.text),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            bool cur_active = ImGui::IsItemActive();
            if (submitted || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                miscmap->dirty     = true;
                s_misc_inline_edit = -1;
            } else if (!cur_active && s_misc_ie_was_active) {
                miscmap->dirty     = true;
                s_misc_inline_edit = -1;
            }
            s_misc_ie_was_active = cur_active;
        } else {
            s_misc_ie_prev_idx   = -1;
            s_misc_ie_was_active = false;
        }
    }

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

        // Panel background (ctx_y = ba_y + ba_h, computed in layout above)
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
            undo_push(undo, beatmap, lyricmap);
            beatmap_fill(beatmap, t1, t2, (double)s_ctx_bpm);
        }
        s_ctx_hover = ImGui::IsItemHovered();
    } else {
        // Pair is no longer selected — invalidate cached indices
        s_ctx_prev0 = s_ctx_prev1 = -1;
        s_ctx_hover = false;
    }

    // --- Section kind picker (right-click on a section) ---
    if (ImGui::BeginPopup("##sec_kind")) {
        int i = s_sec_kind_popup;
        if (i >= 0 && i < sectionmap->count) {
            Section& sec = sectionmap->sections[i];
            ImGui::TextDisabled("Section kind");
            ImGui::Separator();
            for (int k = 0; k < SK_COUNT; k++) {
                bool is_cur = (sec.kind == (SectionKind)k);
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::ColorConvertU32ToFloat4(s_sec_border[k]));
                bool picked = ImGui::Selectable(SECTION_KIND_NAMES[k], is_cur);
                ImGui::PopStyleColor();
                if (picked) {
                    if (!is_cur) {
                        undo_push(undo, nullptr, nullptr, sectionmap, nullptr);
                        sec.kind          = (SectionKind)k;
                        sectionmap->dirty = true;
                    }
                    ImGui::CloseCurrentPopup();
                }
            }
        } else {
            ImGui::CloseCurrentPopup();   // the section went away underneath us
        }
        ImGui::EndPopup();
    }

    // --- Pane checkboxes (left lane, bottom-aligned) ---
    // Table-driven: every strip in the panel registry gets one unlabelled
    // checkbox here; the tooltip says which pane it is.
    {
        dl->AddRectFilled(ImVec2(rx, pane_col_y),
                          ImVec2(rx + pane_lane_w, ry + total_h),
                          IM_COL32(18, 18, 27, 255));
        dl->AddLine(ImVec2(rx, pane_col_y), ImVec2(rx + pane_lane_w, pane_col_y),
                    IM_COL32(45, 45, 62, 255));
        panels_checkbox_column(editor, PK_STRIP, rx + 3.0f, pane_col_y + 1.0f,
                               PANE_ROW_H);
    }

    ImGui::EndChild();

    // --- Lyric Index floating window ---
    editor->lyric_index_open = s_lyric_index_open;
    if (s_lyric_index_open) {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 260.0f), ImGuiCond_FirstUseEver);
        bool wvis = ImGui::Begin("Lyric Index", &s_lyric_index_open);
        if (wvis) {
            double dur      = audio->duration;
            bool   has_audio = (dur > 0.0);

            // Paste button: split clipboard by newlines, add each as an unplaced lyric
            if (ImGui::Button("Paste")) {
                const char* clip = ImGui::GetClipboardText();
                if (clip && clip[0]) {
                    undo_push(undo, beatmap, lyricmap);
                    // Find next available unplaced slot index to avoid t_start collisions
                    double base_t = has_audio ? dur : 0.0;
                    int n_unplaced = 0;
                    for (int k = 0; k < lyricmap->count; k++)
                        if (lyricmap->lyrics[k].t_start >= base_t - 1e-9)
                            n_unplaced++;

                    const char* p = clip;
                    int added = 0;
                    while (*p) {
                        const char* le = p;
                        while (*le && *le != '\n' && *le != '\r') le++;
                        // Trim whitespace from both ends of the line
                        const char* ts = p, *te = le;
                        while (ts < te && (*ts == ' ' || *ts == '\t')) ts++;
                        while (te > ts && (*(te-1) == ' ' || *(te-1) == '\t')) te--;
                        if (te > ts) {
                            char buf[128] = {};
                            int  len = (int)(te - ts);
                            if (len > 127) len = 127;
                            strncpy(buf, ts, (size_t)len);
                            // Place after dur, 1 ms apart, to preserve paste order
                            double t = base_t + (n_unplaced + added) * 0.001;
                            lyricmap_add(lyricmap, t, t + 0.001, buf);
                            added++;
                        }
                        p = le;
                        if (*p == '\r') p++;
                        if (*p == '\n') p++;
                    }
                    // Clipboard held nothing usable — no undo entry for that.
                    if (added == 0) undo_drop_last(undo);
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Add each clipboard line as a new unplaced lyric");
            ImGui::SameLine();
            ImGui::TextDisabled("(%d)", lyricmap->count);

            ImGui::Separator();

            // First unplaced lyric = most likely candidate for the 'L' shortcut
            int first_unplaced_idx = -1;
            if (has_audio) {
                for (int k = 0; k < lyricmap->count; k++) {
                    if (lyricmap->lyrics[k].t_start >= dur - 1e-9) {
                        first_unplaced_idx = k;
                        break;
                    }
                }
            }

            // Deferred auto-place: collect index during loop, act on it after EndChild
            int pending_place        = -1;
            int pending_region_place = -1;
            int pending_delete       = -1;
            int pending_split_idx    = -1;
            int pending_split_cursor = 0;

            // 'L' shortcut: place the first unplaced lyric at the current region
            if (first_unplaced_idx >= 0 && editor->has_region &&
                    ImGui::IsKeyPressed(ImGuiKey_L) && !ImGui::IsAnyItemActive())
                pending_region_place = first_unplaced_idx;

            ImGui::PushFont(s_lyric_font());
            ImGui::BeginChild("##li_rows", ImVec2(0, 0), false);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(2.0f, 2.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, 2.0f));

            const float GW = 22.0f;  // gutter column width

            for (int i = 0; i < lyricmap->count; i++) {
                Lyric& ly     = lyricmap->lyrics[i];
                // A lyric is "placed" when its start time is before audio end.
                // Unplaced lyrics are stored with t_start >= dur (in paste order).
                bool placed   = has_audio && (ly.t_start < dur - 1e-9);
                bool selected = (i == s_lyr_selected);

                ImGui::PushID(i);

                // Highlight row for the currently selected lyric
                if (selected) {
                    ImVec2 rp = ImGui::GetCursorScreenPos();
                    float  rw = ImGui::GetContentRegionAvail().x;
                    float  rh = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        rp, ImVec2(rp.x + rw, rp.y + rh),
                        IM_COL32(40, 80, 145, 88));
                }

                // Gutter button: blue=placed(▶), green=unplaced(+)
                ImVec4 btn_n = placed
                    ? (selected ? ImVec4(0.24f,0.47f,0.78f,0.82f) : ImVec4(0.16f,0.31f,0.55f,0.76f))
                    : (selected ? ImVec4(0.24f,0.51f,0.24f,0.82f) : ImVec4(0.16f,0.35f,0.16f,0.68f));
                ImVec4 btn_h = placed ? ImVec4(0.28f,0.55f,0.86f,0.90f)
                                      : ImVec4(0.27f,0.59f,0.27f,0.90f);
                ImVec4 btn_a = placed ? ImVec4(0.39f,0.67f,1.00f,1.00f)
                                      : ImVec4(0.35f,0.70f,0.35f,1.00f);
                ImGui::PushStyleColor(ImGuiCol_Button,        btn_n);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btn_h);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  btn_a);
                bool hit = ImGui::Button(placed ? ">" : "+", ImVec2(GW, 0));
                ImGui::PopStyleColor(3);

                if (hit) {
                    s_lyr_selected = i;
                    if (placed) {
                        // Scroll timeline so this lyric is visible
                        double span = editor->view_end - editor->view_start;
                        if (span > 0.0) {
                            bool in_view = (ly.t_start >= editor->view_start &&
                                            ly.t_end   <= editor->view_end + 1e-6);
                            if (!in_view) {
                                double vs = ly.t_start - span * 0.25;
                                if (vs < 0.0)                    vs = 0.0;
                                if (vs + span > editor->duration) vs = editor->duration - span;
                                if (vs < 0.0)                    vs = 0.0;
                                editor->view_start = vs;
                                editor->view_end   = vs + span;
                            }
                        }
                    } else if (has_audio) {
                        pending_place = i;
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(placed ? "Scroll to lyric" : "Auto-place on timeline");

                // "Place at region" button – active only for unplaced lyrics when a region exists
                ImGui::SameLine(0, 2.0f);
                {
                    bool can_snap = !placed && editor->has_region;
                    if (!can_snap) ImGui::BeginDisabled();
                    ImGui::PushStyleColor(ImGuiCol_Button,
                        can_snap ? ImVec4(0.45f,0.33f,0.06f,0.82f) : ImVec4(0.22f,0.22f,0.22f,0.40f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.60f,0.46f,0.10f,0.92f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.74f,0.58f,0.16f,1.00f));
                    bool is_next  = (i == first_unplaced_idx) && editor->has_region;
                    bool snap_hit = ImGui::Button(is_next ? "L" : "\xe2\x86\x94", ImVec2(GW, 0));
                    ImGui::PopStyleColor(3);
                    if (!can_snap) ImGui::EndDisabled();
                    if (snap_hit) { s_lyr_selected = i; pending_region_place = i; }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(!placed && !editor->has_region
                            ? "Select a region on the spectrogram first"
                            : placed ? "Already placed" : "Place at selected region");
                }

                // Lyric text input (fills remaining row width, minus X delete button)
                ImGui::SameLine(0, 2.0f);
                const float del_btn_w = 20.0f;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - del_btn_w - 2.0f);
                // Dim unplaced lyrics so placed ones stand out
                if (!placed)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f,0.59f,0.49f,0.76f));
                s_lyr_split_state = {};
                if (ImGui::InputText("##t", ly.text, sizeof(ly.text),
                                     ImGuiInputTextFlags_CallbackAlways, lyr_split_callback))
                    lyricmap->dirty = true;
                if (!placed)
                    ImGui::PopStyleColor();
                // Typing in the text field selects the lyric in the strip too
                if (ImGui::IsItemActive()) s_lyr_selected = i;
                if (s_lyr_split_state.req) {
                    pending_split_idx    = i;
                    pending_split_cursor = s_lyr_split_state.cursor;
                }

                // X button to delete this lyric
                ImGui::SameLine(0, 2.0f);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f,0.12f,0.12f,0.76f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f,0.18f,0.18f,0.90f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.90f,0.25f,0.25f,1.00f));
                if (ImGui::Button("x", ImVec2(del_btn_w, 0))) pending_delete = i;
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete lyric");

                ImGui::PopID();
            }

            if (lyricmap->count == 0)
                ImGui::TextDisabled("No lyrics yet. Drag on the lyric strip or paste from clipboard.");

            ImGui::PopStyleVar(2);
            ImGui::EndChild();
            ImGui::PopFont();

            // ---- Split deferred action ----
            if (pending_split_idx >= 0 && pending_split_idx < lyricmap->count) {
                undo_push(undo, beatmap, lyricmap);
                lyricmap_split(lyricmap, pending_split_idx, pending_split_cursor, &s_lyr_selected);
            }

            // ---- Delete deferred action ----
            if (pending_delete >= 0 && pending_delete < lyricmap->count) {
                undo_push(undo, beatmap, lyricmap);
                lyricmap_remove(lyricmap, pending_delete);
                if (s_lyr_selected == pending_delete)     s_lyr_selected = -1;
                else if (s_lyr_selected > pending_delete) s_lyr_selected--;
            }

            // ---- Auto-place deferred action ----
            // Find a gap in the timeline for this lyric and re-insert with real timestamps.
            if (pending_place >= 0 && pending_place < lyricmap->count) {
                undo_push(undo, beatmap, lyricmap);
                int i = pending_place;

                // prev_end: t_end of last placed lyric before index i (0 if none)
                double prev_end = 0.0;
                for (int j = i - 1; j >= 0; j--) {
                    if (lyricmap->lyrics[j].t_start < dur - 1e-9) {
                        prev_end = lyricmap->lyrics[j].t_end;
                        break;
                    }
                }
                // next_start: t_start of first placed lyric after index i (dur if none)
                double next_start = dur;
                for (int j = i + 1; j < lyricmap->count; j++) {
                    if (lyricmap->lyrics[j].t_start < dur - 1e-9) {
                        next_start = lyricmap->lyrics[j].t_start;
                        break;
                    }
                }

                // Try 2 s window, fall back to 1 s, then fill whatever space exists
                // Small gap after the previous lyric so both handles are easy to grab
                const double GAP = 0.25;
                double t0    = prev_end + GAP;
                double avail = next_start - t0;
                if (avail < 0.05) { t0 = prev_end; avail = next_start - prev_end; }
                double lyr_dur = (avail >= 2.0) ? 2.0 :
                                 (avail >= 1.0) ? 1.0 :
                                 (avail >= 0.05) ? avail : 0.05;
                double t1 = t0 + lyr_dur;

                char saved[128];
                strncpy(saved, lyricmap->lyrics[i].text, sizeof(saved) - 1);
                saved[sizeof(saved) - 1] = '\0';

                lyricmap_remove(lyricmap, i);
                int ni = lyricmap_add(lyricmap, t0, t1, saved);
                s_lyr_selected             = ni;
                lyricmap->selected_idx     = ni;

                // Scroll to the newly placed lyric
                if (ni >= 0 && editor->duration > 0.0) {
                    double span = editor->view_end - editor->view_start;
                    if (span > 0.0) {
                        double vs = t0 - span * 0.25;
                        if (vs < 0.0)                    vs = 0.0;
                        if (vs + span > editor->duration) vs = editor->duration - span;
                        if (vs < 0.0)                    vs = 0.0;
                        editor->view_start = vs;
                        editor->view_end   = vs + span;
                    }
                }
            }

            // ---- Place-at-region deferred action ----
            if (pending_region_place >= 0 && pending_region_place < lyricmap->count
                    && editor->has_region) {
                undo_push(undo, beatmap, lyricmap);
                int    i  = pending_region_place;
                double t0 = editor->region_start;
                double t1 = editor->region_end;

                char saved[128];
                strncpy(saved, lyricmap->lyrics[i].text, sizeof(saved) - 1);
                saved[sizeof(saved) - 1] = '\0';

                lyricmap_remove(lyricmap, i);
                int ni = lyricmap_add(lyricmap, t0, t1, saved);
                s_lyr_selected         = ni;
                lyricmap->selected_idx = ni;
                lyricmap->dirty        = true;

                // Scroll so the newly placed lyric is visible
                if (ni >= 0 && editor->duration > 0.0) {
                    double span = editor->view_end - editor->view_start;
                    if (span > 0.0) {
                        double vs = t0 - span * 0.25;
                        if (vs < 0.0)                     vs = 0.0;
                        if (vs + span > editor->duration)  vs = editor->duration - span;
                        if (vs < 0.0)                     vs = 0.0;
                        editor->view_start = vs;
                        editor->view_end   = vs + span;
                    }
                }
            }
        }
        ImGui::End();
    }
}
