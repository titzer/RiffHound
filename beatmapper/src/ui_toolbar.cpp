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

void ui_toolbar_render(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                       UndoStack* undo, RecentFiles* recent, SectionMap* sectionmap) {
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

    // --- ±5s seek buttons (now in the main flow) ---
    if (ImGui::Button("-5s", ImVec2(40, 0)))
        audio_seek(audio, audio_get_position(audio) - 5.0);
    ImGui::SameLine();
    if (ImGui::Button("+5s", ImVec2(40, 0)))
        audio_seek(audio, audio_get_position(audio) + 5.0);

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

    // --- Speed control (right-aligned) ---
    // Layout: [<<]  0.75x  [>>]
    {
        const float btn_w  = 30.0f;
        const float num_w  = ImGui::CalcTextSize("0.00x").x + 8.0f; // a little padding
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float padding = ImGui::GetStyle().WindowPadding.x;
        float total_w = btn_w + spacing + num_w + spacing + btn_w;
        float right_x = ImGui::GetWindowWidth() - padding - total_w;
        ImGui::SameLine(right_x);

        if (ImGui::Button("<<", ImVec2(btn_w, 0)))
            audio_set_speed(audio, audio->speed - 0.05f);

        ImGui::SameLine();
        char spd_buf[16];
        snprintf(spd_buf, sizeof(spd_buf), "%.2fx", audio->speed);
        // Centre the text within num_w
        float txt_w = ImGui::CalcTextSize(spd_buf).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (num_w - txt_w) * 0.5f);
        ImGui::Text("%s", spd_buf);
        ImGui::SameLine(0, (num_w - txt_w) * 0.5f + spacing);

        if (ImGui::Button(">>", ImVec2(btn_w, 0)))
            audio_set_speed(audio, audio->speed + 0.05f);
    }

    // --- Speed keyboard shortcuts (- / = keys, not captured by a text field) ---
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_Minus, false))
            audio_set_speed(audio, audio->speed - 0.05f);
        // The = key is the unshifted + on a standard keyboard
        if (ImGui::IsKeyPressed(ImGuiKey_Equal, false))
            audio_set_speed(audio, audio->speed + 0.05f);
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
            if (!beatmap_load(beatmap, sectionmap, bm_path))
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
