#include "ui_dock.h"
#include "ui_chroma.h"
#include "ui_beat_detector.h"
#include "ui_smoothing.h"
#include "ui_complete.h"
#include "ui_rhythm.h"
#include "ui_timeline.h"
#include "imgui.h"
#include <float.h>

// --- state -----------------------------------------------------------------

static const float RAIL_W        = 30.0f;
static const float SPLITTER_W    = 5.0f;
static const float DRAWER_MIN_W  = 260.0f;
static const float DRAWER_MAX_W  = 560.0f;
static const float FRAME_PAD     = 4.0f;    // inside the tool frame
static const float FRAME_GAP     = 6.0f;    // between tool frames

static bool  s_drawer_open = false;
static float s_drawer_w    = 320.0f;

struct ToolState {
    bool floating;      // detached into its own window
    bool expanded;      // header triangle state while docked
    bool settings_open; // settings section shown
    bool focus_req;     // bring the floating window to front next frame
    bool opened_req;    // just opened by its rail icon: tools that analyse a selection run now
};
static ToolState s_tools[DOCK_TOOL_COUNT] = {};

// --- tool table --------------------------------------------------------------

static void lyrics_body(ToolCtx& c) {
    ui_timeline_lyric_index_content(c.editor, c.audio, c.beatmap, c.undo, c.lyricmap);
}

// Beats = detector + smoother in one box
static void beats_settings(ToolCtx& c) {
    ImGui::TextDisabled("Detection");
    ui_beat_detector_settings(c);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Smoothing");
    ui_smoothing_settings(c);
}
static void beats_body(ToolCtx& c) {
    ui_beat_detector_body(c);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ui_smoothing_body(c);
}
static void beats_actions(ToolCtx& c) {
    ui_beat_detector_actions(c);
    ui_smoothing_actions(c);
}

struct ToolDesc {
    const char* name;
    ToolFn settings, body, actions;
    int    action_rows;          // rows of buttons the actions part lays out
    bool   body_scrolls;         // body is a scrolling child (else fixed content)
};
static const ToolDesc TOOLS[DOCK_TOOL_COUNT] = {
    { "Chroma Analyzer", ui_chroma_settings,   ui_chroma_body,   nullptr,             0, false },
    { "Beats",           beats_settings,       beats_body,       beats_actions,       3, true  },
    { "Lyric Index",     nullptr,              lyrics_body,      nullptr,             0, false },
    { "Complete Track",  ui_complete_settings, ui_complete_body, ui_complete_actions, 4, false },
    { "Rhythm Map",      ui_rhythm_settings,   ui_rhythm_body,   ui_rhythm_actions,   1, true  },
};

// --- public queries --------------------------------------------------------

float ui_dock_width() {
    return RAIL_W + (s_drawer_open ? s_drawer_w + SPLITTER_W : 0.0f);
}

bool ui_dock_tool_visible(DockTool t) {
    if (t < 0 || t >= DOCK_TOOL_COUNT) return false;
    if (s_tools[t].floating) return true;
    return s_drawer_open && s_tools[t].expanded;
}

const char* ui_dock_tool_name(DockTool t) {
    return (t >= 0 && t < DOCK_TOOL_COUNT) ? TOOLS[t].name : "?";
}

void ui_dock_icon_click(DockTool t) {
    if (t < 0 || t >= DOCK_TOOL_COUNT) return;
    ToolState& ts = s_tools[t];
    if (ts.floating) { ts.focus_req = true; ts.opened_req = true; return; }
    if (s_drawer_open && ts.expanded) { s_drawer_open = false; return; }
    for (int i = 0; i < DOCK_TOOL_COUNT; i++)
        if (i != (int)t && !s_tools[i].floating) s_tools[i].expanded = false;
    s_drawer_open = true;
    ts.expanded   = true;
    ts.opened_req = true;
}

// --- rail icons ------------------------------------------------------------

static void draw_tool_icon(ImDrawList* dl, DockTool t, float cx, float cy, ImU32 col) {
    switch (t) {
    case DOCK_CHROMA:   // three bars of increasing height
        dl->AddRectFilled(ImVec2(cx - 7, cy + 1), ImVec2(cx - 3, cy + 7), col);
        dl->AddRectFilled(ImVec2(cx - 2, cy - 3), ImVec2(cx + 2, cy + 7), col);
        dl->AddRectFilled(ImVec2(cx + 3, cy - 7), ImVec2(cx + 7, cy + 7), col);
        break;
    case DOCK_BEATS: {  // diamond over a flattening wave
        ImVec2 pts[4] = { { cx, cy - 8 }, { cx + 5, cy - 3 }, { cx, cy + 2 }, { cx - 5, cy - 3 } };
        dl->AddConvexPolyFilled(pts, 4, col);
        ImVec2 w[4] = { { cx - 8, cy + 6 }, { cx - 3, cy + 3 }, { cx + 3, cy + 7 }, { cx + 8, cy + 5 } };
        dl->AddPolyline(w, 4, col, 0, 1.5f);
        break;
    }
    case DOCK_LYRICS:   // three text lines
        dl->AddLine(ImVec2(cx - 7, cy - 5), ImVec2(cx + 7, cy - 5), col, 2.0f);
        dl->AddLine(ImVec2(cx - 7, cy),     ImVec2(cx + 7, cy),     col, 2.0f);
        dl->AddLine(ImVec2(cx - 7, cy + 5), ImVec2(cx + 3, cy + 5), col, 2.0f);
        break;
    case DOCK_COMPLETE: // solid bar continued by a dotted one
        dl->AddRectFilled(ImVec2(cx - 8, cy - 3), ImVec2(cx - 1, cy + 3), col);
        for (int i = 0; i < 3; i++)
            dl->AddRectFilled(ImVec2(cx + 1 + i * 3, cy - 3), ImVec2(cx + 3 + i * 3, cy + 3), col);
        dl->AddLine(ImVec2(cx - 8, cy + 6), ImVec2(cx + 8, cy + 6), col, 1.0f);
        break;
    case DOCK_RHYTHM:   // hits of three sizes on a grid
        dl->AddLine(ImVec2(cx - 8, cy + 7), ImVec2(cx + 8, cy + 7), col, 1.0f);
        dl->AddRectFilled(ImVec2(cx - 7, cy - 6), ImVec2(cx - 4, cy + 6), col);
        dl->AddRectFilled(ImVec2(cx - 2, cy + 1), ImVec2(cx + 1, cy + 6), col);
        dl->AddRectFilled(ImVec2(cx + 3, cy - 2), ImVec2(cx + 6, cy + 6), col);
        break;
    default: break;
    }
}

// --- header ------------------------------------------------------------------

static const ImU32 COL_FRAME       = IM_COL32( 88, 100, 150, 255);
static const ImU32 COL_FRAME_DIM   = IM_COL32( 52,  58,  84, 255);
static const ImU32 COL_HDR         = IM_COL32( 30,  32,  50, 255);
static const ImU32 COL_HDR_HOV     = IM_COL32( 40,  44,  68, 255);
static const ImU32 COL_BODY        = IM_COL32( 20,  21,  30, 255);
static const ImU32 COL_SETTINGS    = IM_COL32( 26,  26,  38, 255);

static float header_h() { return ImGui::GetFrameHeight() + 4.0f; }

// Small glyph buttons drawn by hand so the header stays compact.
static bool glyph_button(const char* id, ImVec2 pos, float sz, const char* tip,
                         void (*draw)(ImDrawList*, ImVec2, float, ImU32, bool))
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 80, 120, 120));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(90, 110, 160, 160));
    bool clicked = ImGui::Button(id, ImVec2(sz, sz));
    ImGui::PopStyleColor(3);
    bool hov = ImGui::IsItemHovered();
    if (hov && tip) ImGui::SetTooltip("%s", tip);
    draw(ImGui::GetWindowDrawList(), ImVec2(pos.x + sz * 0.5f, pos.y + sz * 0.5f), sz,
         hov ? IM_COL32(235, 235, 255, 255) : IM_COL32(165, 170, 200, 220), hov);
    return clicked;
}
static void draw_detach_glyph(ImDrawList* dl, ImVec2 c, float, ImU32 col, bool) {
    dl->AddRect(ImVec2(c.x - 5, c.y - 2), ImVec2(c.x + 2, c.y + 5), col, 0.0f, 0, 1.2f);
    dl->AddLine(ImVec2(c.x - 1, c.y - 1), ImVec2(c.x + 5, c.y - 5), col, 1.2f);
    dl->AddLine(ImVec2(c.x + 1, c.y - 5), ImVec2(c.x + 5, c.y - 5), col, 1.2f);
    dl->AddLine(ImVec2(c.x + 5, c.y - 5), ImVec2(c.x + 5, c.y - 1), col, 1.2f);
}
static void draw_dock_glyph(ImDrawList* dl, ImVec2 c, float, ImU32 col, bool) {
    dl->AddRect(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), col, 0.0f, 0, 1.2f);
    dl->AddRectFilled(ImVec2(c.x + 1, c.y - 5), ImVec2(c.x + 5, c.y + 5), col);
}
static void draw_settings_open_glyph(ImDrawList* dl, ImVec2 c, float, ImU32 col, bool) {
    dl->AddTriangleFilled(ImVec2(c.x - 5, c.y - 3), ImVec2(c.x + 5, c.y - 3), ImVec2(c.x, c.y + 4), col);
}
static void draw_settings_closed_glyph(ImDrawList* dl, ImVec2 c, float, ImU32 col, bool) {
    dl->AddTriangleFilled(ImVec2(c.x - 3, c.y - 5), ImVec2(c.x - 3, c.y + 5), ImVec2(c.x + 4, c.y), col);
}

// Header row across the top of a tool frame.  `collapsible` is the drawer
// case (the name toggles expansion); floating windows have a title bar and
// only show the settings triangle + dock button.  Returns true when expanded.
static bool tool_header(DockTool t, bool collapsible, float x, float y, float w) {
    ToolState& ts = s_tools[t];
    const ToolDesc& td = TOOLS[t];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float H   = header_h();
    const float BTN = H - 4.0f;

    ImGui::PushID((int)t);
    bool hov = false;
    if (collapsible) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        float click_w = w - (td.settings ? BTN + 2.0f : 0.0f) - BTN - 6.0f;
        if (ImGui::InvisibleButton("##hdr", ImVec2(click_w, H))) ts.expanded = !ts.expanded;
        hov = ImGui::IsItemHovered();
    }
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + H), hov ? COL_HDR_HOV : COL_HDR);

    // Expansion triangle + name
    float tx = x + 10.0f, ty = y + H * 0.5f;
    if (collapsible) {
        ImU32 tri = IM_COL32(170, 175, 210, 230);
        if (ts.expanded)
            dl->AddTriangleFilled(ImVec2(tx - 5, ty - 3), ImVec2(tx + 5, ty - 3), ImVec2(tx, ty + 4), tri);
        else
            dl->AddTriangleFilled(ImVec2(tx - 3, ty - 5), ImVec2(tx - 3, ty + 5), ImVec2(tx + 4, ty), tri);
    }
    dl->AddText(ImVec2(x + (collapsible ? 20.0f : 8.0f), y + (H - ImGui::GetTextLineHeight()) * 0.5f),
                IM_COL32(222, 225, 245, 255), td.name);

    // Right side: settings triangle (next to the name, at the border), then detach/dock
    float bx = x + w - BTN - 3.0f;
    if (collapsible) {
        if (glyph_button("##detach", ImVec2(bx, y + 2.0f), BTN, "Detach into a floating window",
                         draw_detach_glyph))
            ts.floating = true;
    } else {
        if (glyph_button("##dock", ImVec2(bx, y + 2.0f), BTN, "Return to the side drawer",
                         draw_dock_glyph)) {
            ts.floating = false; ts.expanded = true; s_drawer_open = true;
        }
    }
    bx -= BTN + 2.0f;
    if (td.settings) {
        if (glyph_button("##settings", ImVec2(bx, y + 2.0f), BTN,
                         ts.settings_open ? "Hide settings" : "Settings",
                         ts.settings_open ? draw_settings_open_glyph : draw_settings_closed_glyph))
            ts.settings_open = !ts.settings_open;
    }
    ImGui::PopID();
    return !collapsible || ts.expanded;
}

// --- tool frame ----------------------------------------------------------------

// Lay out one tool inside [x, x+w] x [y, y+h]: header, settings (if open),
// body filling what is left, actions pinned to the bottom.  When h <= 0 the
// frame takes its natural height (collapsed: just the header).
static void tool_frame(DockTool t, ToolCtx& c, float x, float y, float w, float h) {
    ToolState& ts = s_tools[t];
    const ToolDesc& td = TOOLS[t];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float H = header_h();

    bool expanded = tool_header(t, c.in_drawer, x, y, w);
    float bottom  = expanded && h > 0 ? y + h : y + H;

    // Frame border, prominent for the expanded tool
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, bottom), expanded ? COL_FRAME : COL_FRAME_DIM,
                3.0f, 0, expanded ? 2.0f : 1.0f);
    if (!expanded) { ImGui::SetCursorScreenPos(ImVec2(x, bottom + FRAME_GAP)); return; }

    float cur_y = y + H;
    float inner_x = x + FRAME_PAD, inner_w = w - 2.0f * FRAME_PAD;
    ImGui::PushID((int)t + 100);

    // Opened by a click with a selection in place: analyse it straight away.
    // (Chroma and the detector already follow the region on their own.)
    if (ts.opened_req) {
        ts.opened_req = false;
        if (t == DOCK_COMPLETE) ui_complete_auto_analyze(c);
    }

    // Settings
    if (td.settings && ts.settings_open) {
        ImGui::SetCursorScreenPos(ImVec2(inner_x, cur_y + FRAME_PAD));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_SETTINGS);
        float max_h = (bottom - cur_y) * 0.6f;
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, max_h));
        if (ImGui::BeginChild("##settings", ImVec2(inner_w, 0),
                              ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                              ImGuiWindowFlags_None)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 3));
            td.settings(c);
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
        float used = ImGui::GetItemRectSize().y;
        if (used > max_h) used = max_h;
        cur_y += FRAME_PAD + used;
        dl->AddLine(ImVec2(x + 2, cur_y + 2.0f), ImVec2(x + w - 2, cur_y + 2.0f), COL_FRAME_DIM, 1.0f);
        cur_y += 4.0f;
    }

    // Actions reserve their rows at the bottom
    float actions_h = td.actions
        ? td.action_rows * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y) + FRAME_PAD
        : 0.0f;
    float body_h = bottom - cur_y - actions_h - FRAME_PAD;
    if (body_h < 40.0f) body_h = 40.0f;

    // Body
    ImGui::SetCursorScreenPos(ImVec2(inner_x, cur_y + 2.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_BODY);
    ImGuiWindowFlags bflags = td.body_scrolls ? 0 : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (t == DOCK_CHROMA) bflags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild("##body", ImVec2(inner_w, body_h - 2.0f), false, bflags)) {
        if (td.body) td.body(c);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(1 + ((td.settings && ts.settings_open) ? 1 : 0));

    // Actions
    if (td.actions) {
        float ay = bottom - actions_h;
        dl->AddLine(ImVec2(x + 2, ay - 1.0f), ImVec2(x + w - 2, ay - 1.0f), COL_FRAME_DIM, 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(inner_x, ay + 2.0f));
        if (ImGui::BeginChild("##actions", ImVec2(inner_w, actions_h - 2.0f), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            td.actions(c);
        }
        ImGui::EndChild();
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(x, bottom + FRAME_GAP));
}

// --- floating window per tool ----------------------------------------------

static void render_floating(DockTool t, ToolCtx& c) {
    ToolState& ts = s_tools[t];
    ImGui::SetNextWindowSizeConstraints(ImVec2(280, 260), ImVec2(900, 1200));
    ImGui::SetNextWindowSize(ImVec2(t == DOCK_COMPLETE ? 460.0f : 340.0f,
                                    t == DOCK_CHROMA ? 260.0f : 560.0f), ImGuiCond_FirstUseEver);
    if (ts.focus_req) { ImGui::SetNextWindowFocus(); ts.focus_req = false; }

    bool open = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    bool vis = ImGui::Begin(TOOLS[t].name, &open,
                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (vis) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 a = ImGui::GetContentRegionAvail();
        c.in_drawer = false;
        tool_frame(t, c, p.x, p.y, a.x, a.y);
    }
    ImGui::End();
    if (!open) { ts.floating = false; ts.expanded = false; }
}

// --- main render -----------------------------------------------------------

void ui_dock_render(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                    UndoStack* undo, AutoBeatList* autobeat,
                    SectionMap* sectionmap, LyricMap* lyricmap,
                    MiscMap* miscmap, MiscMap* chordmap)
{
    ImGuiIO& io = ImGui::GetIO();
    float dock_w = ui_dock_width();
    float win_h  = io.DisplaySize.y;
    ToolCtx c = { editor, audio, beatmap, undo, autobeat, sectionmap, lyricmap, miscmap, chordmap, true };

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - dock_w, 0));
    ImGui::SetNextWindowSize(ImVec2(dock_w, win_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 16, 24, 255));
    ImGui::Begin("##tooldock", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float rail_x = wp.x + dock_w - RAIL_W;

    dl->AddRectFilled(ImVec2(rail_x, wp.y), ImVec2(wp.x + dock_w, wp.y + win_h), IM_COL32(12, 12, 18, 255));
    dl->AddLine(ImVec2(rail_x, wp.y), ImVec2(rail_x, wp.y + win_h), IM_COL32(50, 50, 70, 255));

    // Rail icon buttons
    {
        const float BTN = 26.0f;
        float bx = rail_x + (RAIL_W - BTN) * 0.5f;
        float by = wp.y + 8.0f;
        for (int t = 0; t < DOCK_TOOL_COUNT; t++) {
            bool active = ui_dock_tool_visible((DockTool)t);
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            ImGui::PushID(t);
            ImGui::PushStyleColor(ImGuiCol_Button,
                active ? IM_COL32(55, 95, 160, 210) : IM_COL32(28, 28, 45, 200));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 120, 190, 230));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(90, 150, 220, 255));
            if (ImGui::Button("##ricon", ImVec2(BTN, BTN)))
                ui_dock_icon_click((DockTool)t);
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TOOLS[t].name);
            ImU32 col = active ? IM_COL32(230, 235, 255, 255) : IM_COL32(160, 160, 195, 220);
            draw_tool_icon(dl, (DockTool)t, bx + BTN * 0.5f, by + BTN * 0.5f, col);
            ImGui::PopID();
            by += BTN + 6.0f;
        }
    }

    // Drawer: splitter + framed tools
    if (s_drawer_open) {
        ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y));
        ImGui::InvisibleButton("##dock_split", ImVec2(SPLITTER_W, win_h));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) {
            s_drawer_w -= io.MouseDelta.x;
            if (s_drawer_w < DRAWER_MIN_W) s_drawer_w = DRAWER_MIN_W;
            if (s_drawer_w > DRAWER_MAX_W) s_drawer_w = DRAWER_MAX_W;
        }
        dl->AddLine(ImVec2(wp.x + SPLITTER_W - 1.0f, wp.y), ImVec2(wp.x + SPLITTER_W - 1.0f, wp.y + win_h),
                    IM_COL32(50, 50, 70, 255));

        // Expanded tools share the height left after every header
        int n_docked = 0, n_exp = 0;
        for (int t = 0; t < DOCK_TOOL_COUNT; t++) {
            if (s_tools[t].floating) continue;
            n_docked++;
            if (s_tools[t].expanded) n_exp++;
        }
        float x = wp.x + SPLITTER_W + FRAME_GAP, w = s_drawer_w - 2.0f * FRAME_GAP;
        float avail = win_h - FRAME_GAP * (n_docked + 1) - header_h() * (n_docked - n_exp);
        float exp_h = n_exp ? avail / n_exp : 0.0f;
        float y = wp.y + FRAME_GAP;
        c.in_drawer = true;
        for (int t = 0; t < DOCK_TOOL_COUNT; t++) {
            if (s_tools[t].floating) continue;
            bool  exp = s_tools[t].expanded;
            float h_t = exp ? exp_h : 0.0f;
            // Chroma is a compact readout: size its frame to the content
            // (status line + 64px bars + note labels, plus settings when
            // open) instead of stretching it to fill the drawer.
            if (t == DOCK_CHROMA && exp) {
                float body_want = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 64.0f + 26.0f;
                float set_want  = s_tools[t].settings_open
                                ? 2.0f * ImGui::GetFrameHeightWithSpacing() + 16.0f : 0.0f;
                float want = header_h() + set_want + body_want + 2.0f * FRAME_PAD;
                if (h_t > want) h_t = want;
            }
            tool_frame((DockTool)t, c, x, y, w, h_t);
            y += (exp ? h_t : header_h()) + FRAME_GAP;
        }
    }

    ImGui::End();

    for (int t = 0; t < DOCK_TOOL_COUNT; t++)
        if (s_tools[t].floating) render_floating((DockTool)t, c);

    // Keep EditorState visibility flags in sync for the rest of the app.
    editor->show_chroma_panel    = ui_dock_tool_visible(DOCK_CHROMA);
    editor->show_beat_detector   = ui_dock_tool_visible(DOCK_BEATS);
    editor->show_smoothing_panel = ui_dock_tool_visible(DOCK_BEATS);
    editor->lyric_index_open     = ui_dock_tool_visible(DOCK_LYRICS);

    if (!editor->show_chroma_panel)    editor->chroma_hover_note = -1;
    if (!editor->show_smoothing_panel) ui_smoothing_hidden();
    if (!ui_dock_tool_visible(DOCK_COMPLETE)) ui_complete_hidden();
}
