#include "ui_help.h"
#include "imgui.h"

// A section header has keys == nullptr.
struct HelpRow {
    const char* keys;
    const char* what;
};

static const HelpRow s_rows[] = {
    { nullptr, "Playback" },
    { "Space",            "Play / stop (stop returns to where playback started)" },
    { "Left / Right",     "Seek 5 seconds back / forward" },
    { "L",                "Toggle loop playback" },
    { "- / =",            "Playback speed down / up by 0.05x" },

    { nullptr, "Beats" },
    { "Click insert strip",       "Place a beat at the cursor" },
    { "Shift+click insert strip", "Place a beat and fill the gap at the nearest tempo" },
    { "Click beat",               "Select a beat; drag to move it" },
    { "Shift+click beat",         "Add / remove a beat from the selection" },
    { "Drag in beats strip",      "Rectangle-select beats" },
    { "Delete / Backspace",       "Delete the selection (beats, then section, then lyric)" },
    { "T",                        "Tap a beat at the playhead while playing" },
    { "I",                        "Insert selected taps and auto-beats as real beats" },

    { nullptr, "Editing" },
    { "Ctrl+Z",           "Undo" },
    { "Ctrl+S",           "Save beatmap" },
    { "Ctrl+O / Cmd+O",   "Open audio file" },
    { "Escape",           "Close a dialog / leave an inline text edit" },
    { "Shift+Enter",      "Split a lyric at the cursor (Lyric Index text field)" },
    { "L",                "Place the next unplaced lyric at the selected region" },

    { nullptr, "View" },
    { "H",                "Show / hide this window" },
    { "Ctrl+Wheel",       "Zoom the timeline around the cursor" },
    { "Horizontal wheel", "Pan the timeline" },
    { "Drag ruler",       "Pan; a click on the ruler seeks" },
    { "Click spectrogram","Seek; drag to select a region for analysis" },
    { "Click minimap",    "Seek anywhere in the track" },
    { "Ctrl+= / Ctrl+-",  "Lyric font larger / smaller" },
    { "Sidebar triangle", "Collapse / expand the beat editor" },
    { "Pane checkboxes",  "Show or hide timeline lanes (left edge, hover for names)" },

    { nullptr, "Annotations" },
    { "Drag section strip",   "Create a section; drag its handles to resize" },
    { "Right-click section",  "Pick its kind (verse, chorus, solo, ...)" },
    { "Double-click section", "Set the section as the loop region" },
    { "Drag lyric strip",     "Create a lyric; double-click to edit its text" },
    { "Drag misc strip",      "Create a free-text annotation" },
};

void ui_help_render(EditorState* editor) {
    if (!editor || !editor->show_help) return;

    ImGui::SetNextWindowSize(ImVec2(560, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Keyboard Shortcuts", &editor->show_help)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Press H to close.");
    ImGui::Spacing();

    const int n_rows = (int)(sizeof(s_rows) / sizeof(s_rows[0]));
    if (ImGui::BeginTable("##help_tbl", 2,
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##keys", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch);

        for (int i = 0; i < n_rows; i++) {
            const HelpRow& r = s_rows[i];
            ImGui::TableNextRow();
            if (!r.keys) {
                // Section header spanning both columns
                ImGui::TableSetColumnIndex(0);
                if (i > 0) ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s", r.what);
                continue;
            }
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImVec4(1.0f, 0.83f, 0.35f, 1.0f), "%s", r.keys);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(r.what);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}
