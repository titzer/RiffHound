#include "ui_toolbar.h"
#include "undo.h"
#include "recent.h"
#include "panels.h"
#include "imgui.h"
#include "platform.h"
#include "ui_beat_detector.h"
#include "ui_complete.h"
#include "ui_timeline.h"
#include <string.h>
#include <stdio.h>

static char s_file_buf[512] = "";
static bool s_show_open_dialog    = false;

void ui_toolbar_open_dialog()   { s_show_open_dialog    = true; }

void ui_toolbar_render(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                       UndoStack* undo, RecentFiles* recent, SectionMap* sectionmap,
                       LyricMap* lyricmap, MiscMap* miscmap, MiscMap* chordmap,
                       AutoBeatList* autobeat, Library* library)
{
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

    // --- Position display (total time lives in the minimap) ---
    if (audio->loaded) {
        double pos = audio_get_position(audio);
        int m = (int)(pos / 60.0);
        double s = pos - m * 60.0;
        ImGui::Text("%d:%06.3f", m, s);
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

    // --- Audio open modal: the library ---
    // A search box over every track in the remembered folders, tagged with
    // what its .txt carries.  Enter (or a click) loads; Browse... opens the
    // system dialog and loads the pick straight away, remembering its folder.
    static char s_query[128]   = {};
    static int  s_highlight    = 0;
    static bool s_want_beats = false, s_want_sections = false, s_want_chords = false,
                s_want_lyrics = false, s_want_loops = false;
    static bool s_focus_search = false;
    if (s_show_open_dialog) {
        ImGui::OpenPopup("Open Audio File");
        s_show_open_dialog = false;
        s_query[0] = 0;
        s_highlight = 0;
        s_focus_search = true;
        library_rescan(library);
    }
    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Open Audio File", nullptr, ImGuiWindowFlags_NoResize)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();

        auto do_load = [&](const char* path) {
            if (!path || !path[0]) return;
            strncpy(s_file_buf, path, sizeof(s_file_buf) - 1);
            undo_clear(undo);
            // Before the old PCM buffer is freed: waits out a Complete Track
            // analysis worker that may still be reading it.
            ui_complete_reset();
            audio_load(audio, editor, s_file_buf);
            char bm_path[512];
            beatmap_path_for_audio(s_file_buf, bm_path, sizeof(bm_path));
            if (!beatmap_load(beatmap, sectionmap, lyricmap, miscmap, chordmap, bm_path))
                beatmap->count = 0;
            strncpy(beatmap->save_path, bm_path, sizeof(beatmap->save_path) - 1);
            beatmap->dirty = false;
            editor->has_region = false;
            ui_beat_detector_reset(autobeat);
            ui_timeline_reset();
            if (sectionmap->count > 0) panel_set_visible(editor, PANEL_SECTIONS, true);
            if (lyricmap->count   > 0) panel_set_visible(editor, PANEL_LYRICS,   true);
            if (miscmap->count    > 0) panel_set_visible(editor, PANEL_MISC,     true);
            if (chordmap->count   > 0) panel_set_visible(editor, PANEL_CHORDS,   true);
            recent_add(recent, s_file_buf);
            recent_save(recent);
            // Remember the folder so the track is in the library next time
            char dir[512];
            strncpy(dir, s_file_buf, sizeof(dir) - 1); dir[sizeof(dir) - 1] = 0;
            char* slash = strrchr(dir, '/');
            if (slash && slash != dir) { *slash = 0; library_add_dir(library, dir); }
            library_touch(library, s_file_buf);
            ImGui::CloseCurrentPopup();
        };

        // Search
        if (s_focus_search) { ImGui::SetKeyboardFocusHere(); s_focus_search = false; }
        ImGui::SetNextItemWidth(-1);
        bool enter = ImGui::InputTextWithHint("##search", "Search tracks (every letter in order)",
                                              s_query, sizeof(s_query),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        bool search_active = ImGui::IsItemActive();
        if (ImGui::IsItemEdited()) s_highlight = 0;

        // Filters
        ImGui::TextDisabled("Has:"); ImGui::SameLine();
        ImGui::Checkbox("Beats", &s_want_beats);       ImGui::SameLine();
        ImGui::Checkbox("Sections", &s_want_sections); ImGui::SameLine();
        ImGui::Checkbox("Chords", &s_want_chords);     ImGui::SameLine();
        ImGui::Checkbox("Lyrics", &s_want_lyrics);     ImGui::SameLine();
        ImGui::Checkbox("Loops", &s_want_loops);

        // Matching entries, most recently opened first, then by title
        static int s_order[4096];
        int n_match = 0;
        for (int i = 0; i < library->count && n_match < 4096; i++) {
            const LibEntry& e = library->entries[i];
            if (!library_fuzzy(s_query, e.title) && !library_fuzzy(s_query, e.dir)) continue;
            if (s_want_beats    && !e.has.beats)    continue;
            if (s_want_sections && !e.has.sections) continue;
            if (s_want_chords   && !e.has.chords)   continue;
            if (s_want_lyrics   && !e.has.lyrics)   continue;
            if (s_want_loops    && !e.has.loops)    continue;
            s_order[n_match++] = i;
        }
        for (int a = 1; a < n_match; a++) {            // insertion sort: lists are small
            int v = s_order[a]; int b = a;
            auto before = [&](int x, int y) {
                const LibEntry& ex = library->entries[x]; const LibEntry& ey = library->entries[y];
                if (ex.last_opened != ey.last_opened) return ex.last_opened > ey.last_opened;
                return strcasecmp(ex.title, ey.title) < 0;
            };
            while (b > 0 && before(v, s_order[b - 1])) { s_order[b] = s_order[b - 1]; b--; }
            s_order[b] = v;
        }
        if (s_highlight >= n_match) s_highlight = n_match ? n_match - 1 : 0;
        if (n_match == library->count) ImGui::TextDisabled("%d tracks in %d folder%s", library->count,
                                                           library->dir_count, library->dir_count == 1 ? "" : "s");
        else ImGui::TextDisabled("%d of %d tracks", n_match, library->count);

        // Keyboard: up/down move the highlight, Enter loads it
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && s_highlight + 1 < n_match) s_highlight++;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,   true) && s_highlight > 0)           s_highlight--;
        if ((enter || (!search_active && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) && n_match > 0)
            do_load(library->entries[s_order[s_highlight]].path);

        // Results: one line per track, presence columns on the right
        static const char* COLS[5] = { "beats", "sect", "chords", "lyrics", "loops" };
        const float COL_W = 52.0f;
        float list_h = ImGui::GetContentRegionAvail().y - 2.0f * ImGui::GetFrameHeightWithSpacing() - 8.0f;
        {
            // Column headers
            float w = ImGui::GetContentRegionAvail().x;
            ImVec2 hp = ImGui::GetCursorScreenPos();
            ImDrawList* hdl = ImGui::GetWindowDrawList();
            for (int c = 0; c < 5; c++) {
                ImVec2 ts = ImGui::CalcTextSize(COLS[c]);
                float cx0 = hp.x + w - (5 - c) * COL_W - 14.0f;
                hdl->AddText(ImVec2(cx0 + (COL_W - ts.x) * 0.5f, hp.y), IM_COL32(140, 140, 170, 220), COLS[c]);
            }
            ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
        }
        if (ImGui::BeginChild("##results", ImVec2(0, list_h), true)) {
            float w = ImGui::GetContentRegionAvail().x;
            for (int r = 0; r < n_match; r++) {
                const LibEntry& e = library->entries[s_order[r]];
                ImGui::PushID(r);
                bool hl = (r == s_highlight);
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                float row_h = ImGui::GetTextLineHeight() + 4.0f;
                if (ImGui::Selectable("##row", hl, 0, ImVec2(0, row_h)))
                    do_load(e.path);
                if (ImGui::IsItemHovered()) {
                    const char* dslash = strrchr(e.dir, '/');
                    ImGui::SetTooltip("%s\nfolder: %s%s", e.path, dslash ? dslash + 1 : e.dir,
                                      e.has_txt ? "" : "\n(no chart)");
                }
                if (hl && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
                    ImGui::SetScrollHereY();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                float ty = p0.y + 2.0f;
                float cols_x = p0.x + w - 5 * COL_W;
                // Title, clipped before the columns
                dl->PushClipRect(ImVec2(p0.x, p0.y), ImVec2(cols_x - 8.0f, p0.y + row_h), true);
                dl->AddText(ImVec2(p0.x + 6.0f, ty), e.has_txt ? IM_COL32(230, 230, 245, 255)
                                                               : IM_COL32(170, 170, 185, 255), e.title);
                dl->PopClipRect();
                // Check marks
                bool has[5] = { e.has.beats, e.has.sections, e.has.chords, e.has.lyrics, e.has.loops };
                for (int c = 0; c < 5; c++) {
                    float cx = cols_x + c * COL_W + COL_W * 0.5f, cy = ty + ImGui::GetTextLineHeight() * 0.5f;
                    if (has[c]) {
                        ImU32 col = IM_COL32(120, 220, 140, 255);
                        dl->AddLine(ImVec2(cx - 5, cy), ImVec2(cx - 1.5f, cy + 4), col, 2.0f);
                        dl->AddLine(ImVec2(cx - 1.5f, cy + 4), ImVec2(cx + 5, cy - 4), col, 2.0f);
                    } else {
                        dl->AddCircleFilled(ImVec2(cx, cy), 1.5f, IM_COL32(70, 70, 90, 200));
                    }
                }
                ImGui::PopID();
            }
            if (n_match == 0) {
                if (library->dir_count == 0)
                    ImGui::TextDisabled("No folders yet. Browse... for a file, or Add folder...");
                else
                    ImGui::TextDisabled("(nothing matches)");
            }
        }
        ImGui::EndChild();

        // Folders
        ImGui::TextDisabled("Folders:");
        for (int i = 0; i < library->dir_count; i++) {
            ImGui::SameLine();
            const char* dslash = strrchr(library->dirs[i], '/');
            const char* dname = dslash ? dslash + 1 : library->dirs[i];
            ImGui::PushID(1000 + i);
            if (ImGui::SmallButton(dname)) { library_remove_dir(library, i); ImGui::PopID(); break; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n(click to forget this folder)", library->dirs[i]);
            ImGui::PopID();
        }

        // Buttons
        if (ImGui::Button("Browse...", ImVec2(110, 0))) {
            char picked[512] = {};
            if (platform_open_file_dialog(picked, sizeof(picked))) do_load(picked);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("System file dialog; the pick loads at once and its folder joins the library");
        ImGui::SameLine();
        if (ImGui::Button("Add folder...", ImVec2(110, 0))) {
            char picked[512] = {};
            if (platform_open_folder_dialog(picked, sizeof(picked))) library_add_dir(library, picked);
        }
        ImGui::SameLine();
        if (ImGui::Button("Rescan", ImVec2(80, 0))) library_rescan(library);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
