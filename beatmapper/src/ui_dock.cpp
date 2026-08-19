#include "ui_dock.h"
#include "ui_chroma.h"
#include "ui_beat_detector.h"
#include "ui_smoothing.h"
#include "ui_timeline.h"
#include "imgui.h"

// --- state -----------------------------------------------------------------

static const float RAIL_W        = 30.0f;
static const float SPLITTER_W    = 5.0f;
static const float DRAWER_MIN_W  = 240.0f;
static const float DRAWER_MAX_W  = 520.0f;

static bool  s_drawer_open = false;
static float s_drawer_w    = 300.0f;

struct ToolState {
    bool floating;   // detached into its own window
    bool expanded;   // header triangle state while docked
    bool focus_req;  // bring the floating window to front next frame
};
static ToolState s_tools[DOCK_TOOL_COUNT] = {
    { false, true, false },
    { false, true, false },
    { false, true, false },
    { false, true, false },
};

static const char* TOOL_NAMES[DOCK_TOOL_COUNT] = {
    "Chroma Analyzer", "Beat Detector", "Beat Smoothing", "Lyric Index",
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

void ui_dock_icon_click(DockTool t) {
    if (t < 0 || t >= DOCK_TOOL_COUNT) return;
    ToolState& ts = s_tools[t];
    if (ts.floating) { ts.focus_req = true; return; }
    if (!s_drawer_open)     { s_drawer_open = true; ts.expanded = true; }
    else if (!ts.expanded)  { ts.expanded = true; }
    else                    { s_drawer_open = false; }
}

// --- rail icons ------------------------------------------------------------

// Small hand-drawn glyphs so the rail reads without text labels.
static void draw_tool_icon(ImDrawList* dl, DockTool t, float cx, float cy, ImU32 col) {
    switch (t) {
    case DOCK_CHROMA:   // three bars of increasing height
        dl->AddRectFilled(ImVec2(cx - 7, cy + 1), ImVec2(cx - 3, cy + 7), col);
        dl->AddRectFilled(ImVec2(cx - 2, cy - 3), ImVec2(cx + 2, cy + 7), col);
        dl->AddRectFilled(ImVec2(cx + 3, cy - 7), ImVec2(cx + 7, cy + 7), col);
        break;
    case DOCK_DETECTOR: {  // diamond (the beat marker)
        ImVec2 pts[4] = { { cx, cy - 7 }, { cx + 7, cy }, { cx, cy + 7 }, { cx - 7, cy } };
        dl->AddConvexPolyFilled(pts, 4, col);
        break;
    }
    case DOCK_SMOOTHING: {  // rough wave flattening into a line
        ImVec2 pts[5] = {
            { cx - 8, cy + 4 }, { cx - 4, cy - 5 }, { cx, cy + 3 },
            { cx + 4, cy - 1 }, { cx + 8, cy },
        };
        dl->AddPolyline(pts, 5, col, 0, 2.0f);
        break;
    }
    case DOCK_LYRICS:   // three text lines
        dl->AddLine(ImVec2(cx - 7, cy - 5), ImVec2(cx + 7, cy - 5), col, 2.0f);
        dl->AddLine(ImVec2(cx - 7, cy),     ImVec2(cx + 7, cy),     col, 2.0f);
        dl->AddLine(ImVec2(cx - 7, cy + 5), ImVec2(cx + 3, cy + 5), col, 2.0f);
        break;
    default: break;
    }
}

// --- header widgets --------------------------------------------------------

// Collapsing header row for a docked tool: triangle + name, and a detach
// button at the right edge.  Returns true when the tool is expanded.
static bool tool_header(DockTool t) {
    ToolState& ts = s_tools[t];
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float ROW_H   = ImGui::GetFrameHeight() + 2.0f;
    const float BTN_W   = 22.0f;
    float row_w = ImGui::GetContentRegionAvail().x;
    ImVec2 p    = ImGui::GetCursorScreenPos();

    // Whole row (minus the detach button) toggles expansion.
    ImGui::PushID((int)t);
    if (ImGui::InvisibleButton("##hdr", ImVec2(row_w - BTN_W - 4.0f, ROW_H)))
        ts.expanded = !ts.expanded;
    bool hdr_hov = ImGui::IsItemHovered();

    dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + row_w, p.y + ROW_H),
                      hdr_hov ? IM_COL32(38, 38, 58, 255) : IM_COL32(28, 28, 42, 255));

    // Triangle
    float tcx = p.x + 10.0f, tcy = p.y + ROW_H * 0.5f;
    ImU32 tri_col = IM_COL32(160, 160, 190, 220);
    if (ts.expanded)
        dl->AddTriangleFilled(ImVec2(tcx - 5.0f, tcy - 3.0f), ImVec2(tcx + 5.0f, tcy - 3.0f),
                              ImVec2(tcx, tcy + 4.0f), tri_col);
    else
        dl->AddTriangleFilled(ImVec2(tcx - 3.0f, tcy - 5.0f), ImVec2(tcx - 3.0f, tcy + 5.0f),
                              ImVec2(tcx + 4.0f, tcy), tri_col);

    dl->AddText(ImVec2(p.x + 20.0f, p.y + (ROW_H - ImGui::GetTextLineHeight()) * 0.5f),
                IM_COL32(210, 210, 230, 255), TOOL_NAMES[t]);

    // Detach button
    ImGui::SetCursorScreenPos(ImVec2(p.x + row_w - BTN_W - 2.0f, p.y + 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(28, 28, 42, 0));
    bool detach = ImGui::Button("##detach", ImVec2(BTN_W, ROW_H - 2.0f));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Detach into a floating window");
    {   // detach glyph: box with an arrow leaving its top-right corner
        float bx = p.x + row_w - BTN_W * 0.5f - 2.0f, by = p.y + ROW_H * 0.5f;
        ImU32 col = ImGui::IsItemHovered() ? IM_COL32(230, 230, 255, 255)
                                           : IM_COL32(150, 150, 180, 200);
        dl->AddRect(ImVec2(bx - 5, by - 2), ImVec2(bx + 2, by + 5), col, 0.0f, 0, 1.2f);
        dl->AddLine(ImVec2(bx - 1, by - 1), ImVec2(bx + 5, by - 5), col, 1.2f);
        dl->AddLine(ImVec2(bx + 1, by - 5), ImVec2(bx + 5, by - 5), col, 1.2f);
        dl->AddLine(ImVec2(bx + 5, by - 5), ImVec2(bx + 5, by - 1), col, 1.2f);
    }
    ImGui::PopID();

    if (detach) ts.floating = true;

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ROW_H + 2.0f));
    return ts.expanded && !ts.floating;
}

// --- tool content dispatch -------------------------------------------------

static void tool_content(DockTool t, bool in_drawer,
                         EditorState* editor, AudioState* audio,
                         BeatMap* beatmap, UndoStack* undo, AutoBeatList* autobeat,
                         SectionMap* sectionmap, LyricMap* lyricmap,
                         MiscMap* miscmap, MiscMap* chordmap)
{
    // In the drawer, tools whose content fills all available vertical space
    // (chroma bars, the lyric list) get a fixed-height child so the tools
    // below them stay reachable; floating windows let them fill the window.
    float boxed_h = in_drawer ? 300.0f : 0.0f;

    switch (t) {
    case DOCK_CHROMA:
        if (ImGui::BeginChild("##chroma_box", ImVec2(0, boxed_h), false))
            ui_chroma_content(editor, audio);
        ImGui::EndChild();
        break;
    case DOCK_DETECTOR:
        ui_beat_detector_content(editor, audio, beatmap, undo, autobeat);
        break;
    case DOCK_SMOOTHING:
        ui_smoothing_content(editor, audio, beatmap, sectionmap, lyricmap,
                             miscmap, chordmap, undo, autobeat);
        break;
    case DOCK_LYRICS:
        if (ImGui::BeginChild("##lyrics_box", ImVec2(0, in_drawer ? 340.0f : 0.0f), false))
            ui_timeline_lyric_index_content(editor, audio, beatmap, undo, lyricmap);
        ImGui::EndChild();
        break;
    default: break;
    }
}

// --- floating window per tool ----------------------------------------------

static void render_floating(DockTool t, EditorState* editor, AudioState* audio,
                            BeatMap* beatmap, UndoStack* undo, AutoBeatList* autobeat,
                            SectionMap* sectionmap, LyricMap* lyricmap,
                            MiscMap* miscmap, MiscMap* chordmap)
{
    ToolState& ts = s_tools[t];

    switch (t) {
    case DOCK_CHROMA:
        ImGui::SetNextWindowSizeConstraints(ImVec2(200, 220), ImVec2(600, 800));
        ImGui::SetNextWindowSize(ImVec2(260, 400), ImGuiCond_FirstUseEver);
        break;
    case DOCK_DETECTOR:
        ImGui::SetNextWindowSizeConstraints(ImVec2(240, 300), ImVec2(500, 760));
        ImGui::SetNextWindowSize(ImVec2(280, 430), ImGuiCond_FirstUseEver);
        break;
    case DOCK_SMOOTHING:
        ImGui::SetNextWindowSizeConstraints(ImVec2(300, 300), ImVec2(640, 900));
        ImGui::SetNextWindowSize(ImVec2(340, 500), ImGuiCond_FirstUseEver);
        break;
    case DOCK_LYRICS:
        ImGui::SetNextWindowSize(ImVec2(420, 290), ImGuiCond_FirstUseEver);
        break;
    default: break;
    }
    if (ts.focus_req) { ImGui::SetNextWindowFocus(); ts.focus_req = false; }

    bool open = true;
    ImGuiWindowFlags flags = 0;
    if (t == DOCK_CHROMA)
        flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    bool vis = ImGui::Begin(TOOL_NAMES[t], &open, flags);
    if (vis) {
        if (ImGui::SmallButton("Dock")) {
            ts.floating = false;
            ts.expanded = true;
            s_drawer_open = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Return to the side drawer");
        ImGui::Separator();
        tool_content(t, false, editor, audio, beatmap, undo, autobeat,
                     sectionmap, lyricmap, miscmap, chordmap);
    }
    ImGui::End();

    // Closing the window docks the tool back, collapsed.
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

    // Rail background + separating line
    dl->AddRectFilled(ImVec2(rail_x, wp.y), ImVec2(wp.x + dock_w, wp.y + win_h),
                      IM_COL32(12, 12, 18, 255));
    dl->AddLine(ImVec2(rail_x, wp.y), ImVec2(rail_x, wp.y + win_h),
                IM_COL32(50, 50, 70, 255));

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
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TOOL_NAMES[t]);
            ImU32 col = active ? IM_COL32(230, 235, 255, 255) : IM_COL32(160, 160, 195, 220);
            draw_tool_icon(dl, (DockTool)t, bx + BTN * 0.5f, by + BTN * 0.5f, col);
            ImGui::PopID();
            by += BTN + 6.0f;
        }
    }

    // Drawer: splitter + stacked tools
    if (s_drawer_open) {
        // Resize splitter along the drawer's left edge
        ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y));
        ImGui::InvisibleButton("##dock_split", ImVec2(SPLITTER_W, win_h));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) {
            s_drawer_w -= io.MouseDelta.x;
            if (s_drawer_w < DRAWER_MIN_W) s_drawer_w = DRAWER_MIN_W;
            if (s_drawer_w > DRAWER_MAX_W) s_drawer_w = DRAWER_MAX_W;
        }
        dl->AddLine(ImVec2(wp.x + SPLITTER_W - 1.0f, wp.y),
                    ImVec2(wp.x + SPLITTER_W - 1.0f, wp.y + win_h),
                    IM_COL32(50, 50, 70, 255));

        ImGui::SetCursorScreenPos(ImVec2(wp.x + SPLITTER_W, wp.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
        if (ImGui::BeginChild("##drawer", ImVec2(s_drawer_w, win_h), false)) {
            for (int t = 0; t < DOCK_TOOL_COUNT; t++) {
                if (s_tools[t].floating) continue;
                if (tool_header((DockTool)t)) {
                    tool_content((DockTool)t, true, editor, audio, beatmap, undo, autobeat,
                                 sectionmap, lyricmap, miscmap, chordmap);
                    ImGui::Spacing();
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    ImGui::End();

    // Detached floating tools
    for (int t = 0; t < DOCK_TOOL_COUNT; t++)
        if (s_tools[t].floating)
            render_floating((DockTool)t, editor, audio, beatmap, undo, autobeat,
                            sectionmap, lyricmap, miscmap, chordmap);

    // Keep EditorState visibility flags in sync for the rest of the app
    // (chroma hover overlay, the 'L' shortcut, menu checkmarks).
    editor->show_chroma_panel    = ui_dock_tool_visible(DOCK_CHROMA);
    editor->show_beat_detector   = ui_dock_tool_visible(DOCK_DETECTOR);
    editor->show_smoothing_panel = ui_dock_tool_visible(DOCK_SMOOTHING);
    editor->lyric_index_open     = ui_dock_tool_visible(DOCK_LYRICS);

    if (!editor->show_chroma_panel)    editor->chroma_hover_note = -1;
    if (!editor->show_smoothing_panel) ui_smoothing_hidden();
}
