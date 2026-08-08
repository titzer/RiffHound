#include "panels.h"
#include "imgui.h"

const PanelDesc PANELS[PANEL_COUNT] = {
    { PANEL_INSERT, PK_STRIP, "Insert", "Beat insertion strip",
      "Click to place a beat; shift+click to place and fill", nullptr,
      &EditorState::show_place_strip },
    { PANEL_BEATS, PK_STRIP, "Beats", "Beats strip",
      "Beat markers: select, drag, rectangle-select", nullptr,
      &EditorState::show_beat_strip },
    { PANEL_TEMPO, PK_STRIP, "Tempo", "Tempo graph",
      "Instantaneous and rolling-average BPM behind the beats strip", nullptr,
      &EditorState::show_tempo_graph },
    { PANEL_TAPS, PK_STRIP, "Taps", "Tap strip",
      "Taps recorded with the T key during playback", nullptr,
      &EditorState::show_tap_strip },
    { PANEL_AUTO, PK_STRIP, "Auto", "Auto-beat strip",
      "Beats proposed by the Beat Detector", nullptr,
      &EditorState::show_autobeat_strip },
    { PANEL_SECTIONS, PK_STRIP, "Sect", "Sections strip",
      "Song sections: drag to create, double-click to loop", nullptr,
      &EditorState::show_section_strip },
    { PANEL_LYRICS, PK_STRIP, "Lyrics", "Lyrics strip",
      "Lyric lines and the Lyric Index", nullptr,
      &EditorState::show_lyric_strip },
    { PANEL_CHORDS, PK_STRIP, "Chord", "Chords strip",
      "Chord names; the 'chord:' keyword is added on save", nullptr,
      &EditorState::show_chord_strip },
    { PANEL_MISC, PK_STRIP, "Misc", "Misc annotations strip",
      "Free-text annotations, e.g. 'strum: DuDuUuDu'", nullptr,
      &EditorState::show_misc_strip },

    { PANEL_CHROMA, PK_WINDOW, "Chroma", "Chroma Analyzer",
      "Pitch-class energy at the playhead", nullptr,
      &EditorState::show_chroma_panel },
    { PANEL_DETECTOR, PK_WINDOW, "Detect", "Beat Detector",
      "Automatic beat detection over the selected region", nullptr,
      &EditorState::show_beat_detector },
    { PANEL_SMOOTHING, PK_WINDOW, "Smooth", "Beat Smoothing",
      "Even out the tempo across a range of selected beats", nullptr,
      &EditorState::show_smoothing_panel },
    { PANEL_HELP, PK_HELP, "Help", "Keyboard Shortcuts",
      "List of keyboard and mouse shortcuts", "H",
      &EditorState::show_help },
};

bool panel_visible(const EditorState* e, PanelId id) {
    if (!e || id < 0 || id >= PANEL_COUNT) return false;
    return e->*(PANELS[id].flag);
}

void panel_set_visible(EditorState* e, PanelId id, bool v) {
    if (!e || id < 0 || id >= PANEL_COUNT) return;
    e->*(PANELS[id].flag) = v;
}

void panel_toggle(EditorState* e, PanelId id) {
    if (!e || id < 0 || id >= PANEL_COUNT) return;
    e->*(PANELS[id].flag) = !(e->*(PANELS[id].flag));
}

int panels_count(PanelKind kind) {
    int n = 0;
    for (int i = 0; i < PANEL_COUNT; i++)
        if (PANELS[i].kind == kind) n++;
    return n;
}

void panels_checkbox_column(EditorState* e, PanelKind kind,
                            float x, float y_top, float row_h)
{
    if (!e) return;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
    float y = y_top;
    for (int i = 0; i < PANEL_COUNT; i++) {
        const PanelDesc& p = PANELS[i];
        if (p.kind != kind) continue;
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::Checkbox("##pane", &(e->*(p.flag)));
        if (ImGui::IsItemHovered()) {
            if (p.tip) ImGui::SetTooltip("%s\n%s", p.name, p.tip);
            else       ImGui::SetTooltip("%s", p.name);
        }
        ImGui::PopID();
        y += row_h;
    }
    ImGui::PopStyleVar();
}

void panels_checkbox_row(EditorState* e, PanelKind kind) {
    if (!e) return;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(6.0f, 2.0f));
    bool first = true;
    for (int i = 0; i < PANEL_COUNT; i++) {
        const PanelDesc& p = PANELS[i];
        if (p.kind != kind) continue;
        if (!first) ImGui::SameLine();
        first = false;
        ImGui::PushID(i);
        ImGui::Checkbox(p.label, &(e->*(p.flag)));
        if (p.tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.tip);
        ImGui::PopID();
    }
    ImGui::PopStyleVar(2);
}

void panels_checkbox_list(EditorState* e, PanelKind kind) {
    if (!e) return;
    for (int i = 0; i < PANEL_COUNT; i++) {
        const PanelDesc& p = PANELS[i];
        if (p.kind != kind) continue;
        ImGui::PushID(i);
        ImGui::Checkbox(p.name, &(e->*(p.flag)));
        if (p.tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.tip);
        ImGui::PopID();
    }
}

void panels_menu_items(EditorState* e, PanelKind kind) {
    if (!e) return;
    for (int i = 0; i < PANEL_COUNT; i++) {
        const PanelDesc& p = PANELS[i];
        if (p.kind != kind) continue;
        ImGui::MenuItem(p.name, p.shortcut, &(e->*(p.flag)));
    }
}
