#include "ui_toolbar.h"
#include "undo.h"
#include "recent.h"
#include "imgui.h"
#include "platform.h"
#include <string.h>
#include <stdio.h>

static char s_file_buf[512] = "";
static bool s_show_open_dialog = false;

void ui_toolbar_open_dialog() { s_show_open_dialog = true; }

static ImVec4 kActiveColor = { 0.3f, 0.5f, 0.8f, 1.0f };

static void tool_button(const char* label, ToolMode mode, EditorState* editor) {
    bool active = (editor->tool_mode == mode);
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, kActiveColor);
    if (ImGui::Button(label)) editor->tool_mode = mode;
    if (active) ImGui::PopStyleColor();
}

void ui_toolbar_render(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                       UndoStack* undo, RecentFiles* recent) {
    // --- Tool mode buttons ---
    ImGui::Text("Tool:");
    ImGui::SameLine();
    tool_button("Select",      ToolMode::Select,      editor);
    ImGui::SameLine();
    tool_button("Interpolate", ToolMode::Interpolate, editor);

    // --- Interpolate panel (only when active) ---
    if (editor->tool_mode == ToolMode::Interpolate) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // BPM input – wide enough for "0000.0" plus the two step-arrow buttons
        ImGui::Text("BPM:");
        ImGui::SameLine();
        float bpm_f = (float)editor->bpm;
        float bpm_w = ImGui::CalcTextSize("0000.0").x
                    + ImGui::GetStyle().FramePadding.x   * 2.0f
                    + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f
                    + ImGui::GetFrameHeight()             * 2.0f;
        ImGui::SetNextItemWidth(bpm_w);
        if (ImGui::InputFloat("##bpm", &bpm_f, 0.5f, 5.0f, "%.1f"))
            editor->bpm = (bpm_f < 1.0f) ? 1.0 : (double)bpm_f;
        ImGui::SameLine();

        // Show computed BPM when 3+ beats are selected
        int    n_sel    = beatmap_selected_count(beatmap);
        double computed = (n_sel >= 3) ? beatmap_selected_bpm(beatmap) : 0.0;
        if (computed > 0.0) {
            ImGui::Text("(%.1f computed)", computed);
            ImGui::SameLine();
        } else if (n_sel > 0) {
            ImGui::TextDisabled("(%d sel)", n_sel);
            ImGui::SameLine();
        }

        // Fill button
        if (n_sel < 2) ImGui::BeginDisabled();
        if (ImGui::Button("Fill")) {
            undo_push(undo, beatmap);
            // Snapshot selected times before any insertions shift indices
            static double sel_t[4096];
            int sel_n = 0;
            for (int i = 0; i < beatmap->count && sel_n < 4096; i++)
                if (beatmap->beats[i].selected)
                    sel_t[sel_n++] = beatmap->beats[i].time;

            if (sel_n >= 3) {
                // Compute average BPM from selected beats and update the field
                double span = sel_t[sel_n - 1] - sel_t[0];
                if (span > 0.0)
                    editor->bpm = 60.0 * (sel_n - 1) / span;
            }

            // Fill between each consecutive pair of selected beats
            for (int i = 0; i < sel_n - 1; i++)
                beatmap_fill(beatmap, sel_t[i], sel_t[i + 1], editor->bpm);
        }
        if (n_sel < 2) ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // --- Playback controls ---
    bool can_play = audio->loaded && !audio->playing;
    bool can_stop = audio->loaded &&  audio->playing;

    if (!can_play) ImGui::BeginDisabled();
    if (ImGui::Button("Play")) {
        if (editor->has_region)
            audio_seek(audio, editor->region_start);
        audio_play(audio);
    }
    if (!can_play) ImGui::EndDisabled();

    ImGui::SameLine();

    if (!can_stop) ImGui::BeginDisabled();
    if (ImGui::Button("Stop")) {
        audio_pause(audio);
        audio_seek(audio, audio->play_start);
    }
    if (!can_stop) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // --- Position display ---
    if (audio->loaded) {
        double pos = audio_get_position(audio);
        int m = (int)(pos / 60.0);
        double s = pos - m * 60.0;
        ImGui::Text("%d:%06.3f", m, s);
        ImGui::SameLine();
        ImGui::Text("/ %.1fs", audio->duration);
    } else {
        ImGui::TextDisabled("--:---.---");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // --- File open ---
    if (ImGui::Button("Open...")) {
        s_show_open_dialog = true;
        if (audio->loaded)
            strncpy(s_file_buf, audio->filename, sizeof(s_file_buf) - 1);
    }

    if (audio->loaded) {
        ImGui::SameLine();
        const char* slash = strrchr(audio->filename, '/');
        const char* name  = slash ? slash + 1 : audio->filename;
        ImGui::TextDisabled("%s", name);
    }
    if (beatmap->dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.1f, 1.0f), "*");
    }

    // --- Right-aligned ±5s seek buttons ---
    {
        float btn_w   = 48.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float padding = ImGui::GetStyle().WindowPadding.x;
        float right_x = ImGui::GetWindowWidth() - padding - btn_w * 2 - spacing;
        ImGui::SameLine(right_x);
        if (ImGui::Button("-5s", ImVec2(btn_w, 0)))
            audio_seek(audio, audio_get_position(audio) - 5.0);
        ImGui::SameLine();
        if (ImGui::Button("+5s", ImVec2(btn_w, 0)))
            audio_seek(audio, audio_get_position(audio) + 5.0);
    }

    // --- Audio open modal ---
    if (s_show_open_dialog) {
        ImGui::OpenPopup("Open Audio File");
        s_show_open_dialog = false;
    }
    if (ImGui::BeginPopupModal("Open Audio File", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();

        // Helper: load the file in s_file_buf, update recent list, close popup
        auto do_load = [&]() {
            if (s_file_buf[0] == '\0') return;
            audio_load(audio, s_file_buf);
            char bm_path[512];
            beatmap_path_for_audio(s_file_buf, bm_path, sizeof(bm_path));
            if (!beatmap_load(beatmap, bm_path))
                beatmap->count = 0;
            // Companion .txt is the default save target regardless of whether it exists
            strncpy(beatmap->save_path, bm_path, sizeof(beatmap->save_path) - 1);
            beatmap->dirty = false;
            recent_add(recent, s_file_buf);
            recent_save(recent);
            ImGui::CloseCurrentPopup();
        };

        // Recent files list
        if (recent->count > 0) {
            ImGui::TextDisabled("Recent:");
            for (int i = 0; i < recent->count; i++) {
                const char* slash = strrchr(recent->paths[i], '/');
                const char* name  = slash ? slash + 1 : recent->paths[i];
                ImGui::PushID(i);
                if (ImGui::Selectable(name, false, 0, ImVec2(400, 0))) {
                    strncpy(s_file_buf, recent->paths[i], sizeof(s_file_buf) - 1);
                    do_load();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", recent->paths[i]);
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::Spacing();
        }

        // Manual entry
        ImGui::Text("Path to .mp3 or .wav:");
        ImGui::SetNextItemWidth(360);
        bool enter_pressed = ImGui::InputText("##path", s_file_buf, sizeof(s_file_buf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            char picked[512] = {};
            if (platform_open_file_dialog(picked, sizeof(picked)))
                strncpy(s_file_buf, picked, sizeof(s_file_buf) - 1);
        }
        ImGui::Spacing();
        if (ImGui::Button("Load", ImVec2(80, 0)) || enter_pressed)
            do_load();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
