#include "ui_toolbar.h"
#include "imgui.h"
#include "platform.h"
#include <string.h>
#include <stdio.h>

// Simple file-path input dialog state
static char s_file_buf[512] = "";
static bool s_show_open_dialog = false;

void ui_toolbar_render(EditorState* editor, AudioState* audio) {
    // Tool mode buttons
    ImGui::Text("Tool:");
    ImGui::SameLine();
    bool sel  = (editor->tool_mode == ToolMode::Select);
    bool plc  = (editor->tool_mode == ToolMode::Place);
    bool rgn  = (editor->tool_mode == ToolMode::RegionSelect);

    if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
    if (ImGui::Button("Select")) editor->tool_mode = ToolMode::Select;
    if (sel) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (plc) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
    if (ImGui::Button("Place")) editor->tool_mode = ToolMode::Place;
    if (plc) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (rgn) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
    if (ImGui::Button("Region")) editor->tool_mode = ToolMode::RegionSelect;
    if (rgn) ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Playback controls
    bool can_play = audio->loaded && !audio->playing;
    bool can_stop = audio->loaded &&  audio->playing;

    if (!can_play) ImGui::BeginDisabled();
    if (ImGui::Button("Play")) audio_play(audio);
    if (!can_play) ImGui::EndDisabled();

    ImGui::SameLine();

    if (!can_stop) ImGui::BeginDisabled();
    if (ImGui::Button("Stop")) {
        audio_pause(audio);
        audio_seek(audio, 0.0);
    }
    if (!can_stop) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Position display
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

    // File open
    if (ImGui::Button("Open...")) {
        s_show_open_dialog = true;
        if (audio->loaded)
            strncpy(s_file_buf, audio->filename, sizeof(s_file_buf) - 1);
    }

    if (audio->loaded) {
        ImGui::SameLine();
        // Show just the filename portion
        const char* slash = strrchr(audio->filename, '/');
        const char* name  = slash ? slash + 1 : audio->filename;
        ImGui::TextDisabled("%s", name);
    }

    // Right-aligned ±5s seek buttons
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

    // Simple path input modal
    if (s_show_open_dialog) {
        ImGui::OpenPopup("Open Audio File");
        s_show_open_dialog = false;
    }
    if (ImGui::BeginPopupModal("Open Audio File", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::Text("Enter path to .mp3 or .wav:");
        ImGui::SetNextItemWidth(360);
        ImGui::InputText("##path", s_file_buf, sizeof(s_file_buf));
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            char picked[512] = {};
            if (platform_open_file_dialog(picked, sizeof(picked)))
                strncpy(s_file_buf, picked, sizeof(s_file_buf) - 1);
        }
        ImGui::Spacing();
        if (ImGui::Button("Load", ImVec2(80, 0))) {
            if (s_file_buf[0] != '\0')
                audio_load(audio, s_file_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
