#include "ui_toolbar.h"
#include "undo.h"
#include "recent.h"
#include "panels.h"
#include "imgui.h"
#include "platform.h"
#include <string.h>
#include <stdio.h>

static char s_file_buf[512] = "";
static bool s_show_open_dialog    = false;

void ui_toolbar_open_dialog()   { s_show_open_dialog    = true; }

void ui_toolbar_render(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                       UndoStack* undo, RecentFiles* recent, SectionMap* sectionmap,
                       LyricMap* lyricmap, MiscMap* miscmap, MiscMap* chordmap) {
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

    // Loop toggle button — highlighted (blue) when active.
    // Snapshot before Button() so push/pop counts always match regardless of
    // whether the click toggles the flag inside the same frame.
    bool loop_active = audio->loop;
    if (loop_active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.40f, 0.75f, 1.0f));
    }
    if (ImGui::Button("Loop"))
        audio->loop = !audio->loop;
    if (loop_active)
        ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggle loop playback (L)");

    ImGui::SameLine();

    // Autoscroll toggle button — highlighted (blue) when active.
    bool follow_active = editor->autoscroll;
    if (follow_active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.40f, 0.75f, 1.0f));
    }
    if (ImGui::Button("Follow"))
        editor->autoscroll = !editor->autoscroll;
    if (follow_active)
        ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scroll timeline with playback");

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

    // --- Tuning popup: semitone and cent pitch adjustment ---
    ImGui::SameLine();
    {
        bool pitch_active = (editor->semitones != 0 || editor->cents != 0);
        if (pitch_active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.55f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.40f, 0.75f, 1.0f));
        }
        if (ImGui::Button("Tuning"))
            ImGui::OpenPopup("##tuning");
        if (pitch_active)
            ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pitch shift: %+d semitones, %+d cents",
                              editor->semitones, editor->cents);

        if (ImGui::BeginPopup("##tuning")) {
            const float btn_w = 26.0f;
            const float val_w = ImGui::CalcTextSize("-12 semitones").x + 10.0f;

            ImGui::TextDisabled("Tuning");
            ImGui::Separator();

            // Semitones: [-] value [+]
            if (ImGui::Button("-##st", ImVec2(btn_w, 0)))
                audio_set_pitch(editor, editor->semitones - 1, editor->cents);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pitch: -1 semitone");
            ImGui::SameLine();
            {
                char buf[24];
                snprintf(buf, sizeof(buf), "%+d semitones", editor->semitones);
                float tw = ImGui::CalcTextSize(buf).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (val_w - tw) * 0.5f);
                if (editor->semitones != 0)
                    ImGui::TextColored(ImVec4(0.45f, 0.70f, 1.0f, 1.0f), "%s", buf);
                else
                    ImGui::Text("%s", buf);
                ImGui::SameLine(0, (val_w - tw) * 0.5f + ImGui::GetStyle().ItemSpacing.x);
            }
            if (ImGui::Button("+##st", ImVec2(btn_w, 0)))
                audio_set_pitch(editor, editor->semitones + 1, editor->cents);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pitch: +1 semitone");

            // Cents: [-] value [+]
            if (ImGui::Button("-##ct", ImVec2(btn_w, 0)))
                audio_set_pitch(editor, editor->semitones, editor->cents - 1);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pitch: -1 cent");
            ImGui::SameLine();
            {
                char buf[24];
                snprintf(buf, sizeof(buf), "%+d cents", editor->cents);
                float tw = ImGui::CalcTextSize(buf).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (val_w - tw) * 0.5f);
                if (editor->cents != 0)
                    ImGui::TextColored(ImVec4(0.45f, 0.70f, 1.0f, 1.0f), "%s", buf);
                else
                    ImGui::Text("%s", buf);
                ImGui::SameLine(0, (val_w - tw) * 0.5f + ImGui::GetStyle().ItemSpacing.x);
            }
            if (ImGui::Button("+##ct", ImVec2(btn_w, 0)))
                audio_set_pitch(editor, editor->semitones, editor->cents + 1);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pitch: +1 cent");

            bool at_default = (editor->semitones == 0 && editor->cents == 0);
            if (at_default) ImGui::BeginDisabled();
            if (ImGui::Button("Reset", ImVec2(btn_w * 2 + val_w + ImGui::GetStyle().ItemSpacing.x * 2, 0)))
                audio_set_pitch(editor, 0, 0);
            if (at_default) ImGui::EndDisabled();

            ImGui::EndPopup();
        }
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
    // Layout:  [-] 0.75x [+]
    {
        const float btn_w     = 30.0f;
        const float spd_num_w = ImGui::CalcTextSize("0.00x").x   + 8.0f;
        const float spacing   = ImGui::GetStyle().ItemSpacing.x;
        const float padding   = ImGui::GetStyle().WindowPadding.x;

        float total_w = btn_w + spacing + spd_num_w + spacing + btn_w;
        float right_x = ImGui::GetWindowWidth() - padding - total_w;
        ImGui::SameLine(right_x);

        if (ImGui::Button("-", ImVec2(btn_w, 0)))
            audio_set_speed(editor, editor->speed - 0.05f);

        ImGui::SameLine();
        char spd_buf[16];
        snprintf(spd_buf, sizeof(spd_buf), "%.2fx", editor->speed);
        float spd_txt_w = ImGui::CalcTextSize(spd_buf).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (spd_num_w - spd_txt_w) * 0.5f);
        ImGui::Text("%s", spd_buf);
        ImGui::SameLine(0, (spd_num_w - spd_txt_w) * 0.5f + spacing);

        if (ImGui::Button("+", ImVec2(btn_w, 0)))
            audio_set_speed(editor, editor->speed + 0.05f);
    }

    // --- Speed keyboard shortcuts (- / = keys, not captured by a text field) ---
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_Minus, false))
            audio_set_speed(editor, editor->speed - 0.05f);
        // The = key is the unshifted + on a standard keyboard
        if (ImGui::IsKeyPressed(ImGuiKey_Equal, false))
            audio_set_speed(editor, editor->speed + 0.05f);
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
            // History belongs to the track being replaced — drop it, or Ctrl+Z
            // would paste the previous track's beats over the new one.
            undo_clear(undo);
            audio_load(audio, editor, s_file_buf);
            char bm_path[512];
            beatmap_path_for_audio(s_file_buf, bm_path, sizeof(bm_path));
            if (!beatmap_load(beatmap, sectionmap, lyricmap, miscmap, chordmap, bm_path))
                beatmap->count = 0;
            // Companion .txt is the default save target regardless of whether it exists
            strncpy(beatmap->save_path, bm_path, sizeof(beatmap->save_path) - 1);
            beatmap->dirty = false;
            editor->has_region = false;
            // Auto-show strips that have content in the loaded file
            if (sectionmap->count > 0) panel_set_visible(editor, PANEL_SECTIONS, true);
            if (lyricmap->count   > 0) panel_set_visible(editor, PANEL_LYRICS,   true);
            if (miscmap->count    > 0) panel_set_visible(editor, PANEL_MISC,     true);
            if (chordmap->count   > 0) panel_set_visible(editor, PANEL_CHORDS,   true);
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
