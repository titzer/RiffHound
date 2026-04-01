#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include "audio.h"
#include "spectrogram.h"
#include "editor.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "undo.h"
#include "recent.h"
#include "ui_timeline.h"
#include "ui_toolbar.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Derive a suggested beatmap filename from an audio filepath.
// e.g. "/path/to/track.mp3" → "track.txt"
static void beatmap_suggested_name(const char* audio_path, char* out, int out_size) {
    const char* name = strrchr(audio_path, '/');
    name = name ? name + 1 : audio_path;
    const char* dot = strrchr(name, '.');
    int stem = dot ? (int)(dot - name) : (int)strlen(name);
    if (stem > out_size - 5) stem = out_size - 5;
    strncpy(out, name, stem);
    strcpy(out + stem, ".txt");
}

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

int main(int argc, char** argv) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to init GLFW\n");
        return 1;
    }

    // OpenGL 3.3 core profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1280, 800, "Beatmap Editor", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // NavEnableKeyboard is intentionally not set: it would make Space activate
    // the focused button, conflicting with our global play/stop shortcut.

    ImGui::StyleColorsDark();

    // Slightly adjust the style for a more editor-like look
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 0.0f;
    style.FrameRounding    = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.14f, 1.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Application state
    AudioState       audio;
    SpectrogramState spectro;
    EditorState      editor;
    BeatMap          beatmap;
    SectionMap       sectionmap;
    UndoStack        undo;
    RecentFiles      recent;

    audio_init(&audio);
    spectrogram_init(&spectro);
    editor_init(&editor);
    beatmap_init(&beatmap);
    sectionmap_init(&sectionmap);
    undo_init(&undo);
    recent_init(&recent);
    recent_load(&recent);

    // If files were passed on the command line, add all existing ones to the
    // recent list and open the last valid file.
    if (argc >= 2) {
        const char* last_file = nullptr;
        for (int i = 1; i < argc; i++) {
            FILE* probe = fopen(argv[i], "rb");
            if (probe) {
                fclose(probe);
                recent_add(&recent, argv[i]);
                last_file = argv[i];
            }
        }
        if (last_file) {
            recent_save(&recent);
            audio_load(&audio, last_file);
            char bm_path[512];
            beatmap_path_for_audio(last_file, bm_path, sizeof(bm_path));
            if (!beatmap_load(&beatmap, &sectionmap, bm_path))
                beatmap.count = 0;
            strncpy(beatmap.save_path, bm_path, sizeof(beatmap.save_path) - 1);
            beatmap.dirty = false;
        }
    }

    static bool show_demo       = false;
    static bool show_quit_modal = false;

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Intercept window-close when there are unsaved changes.
        if (glfwWindowShouldClose(window) && beatmap.dirty) {
            glfwSetWindowShouldClose(window, 0);
            show_quit_modal = true;
        }

        // Sync position and playing state from the audio thread.
        audio_update(&audio);

        // Auto-stop when playhead reaches the end of the region selection.
        // Just pause; leave the playhead at region_end so the user can see where
        // they are. Play / Space will seek back to region_start automatically.
        if (audio.playing && editor.has_region &&
            audio.position >= editor.region_end) {
            audio_pause(&audio);
        }

        // Recompute spectrogram whenever a new file is loaded.
        // Detected by comparing the duration the spectrogram was last built for
        // against the current audio duration.
        if (audio.loaded && spectro.duration != audio.duration) {
            float*   pcm     = nullptr;
            uint64_t nframes = 0;
            uint32_t sr      = 0;
            if (audio_decode_pcm(audio.filename, &pcm, &nframes, &sr)) {
                spectrogram_compute(&spectro, pcm, nframes, sr);
                audio_free_pcm(pcm);
            }
            // Always update these so we don't retry endlessly on decode failure.
            spectro.duration = audio.duration;
            editor.duration  = audio.duration;
            editor.view_end  = (audio.duration < 30.0) ? audio.duration : 30.0;
            editor_clamp_view(&editor);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Global shortcuts — checked after NewFrame, blocked only when a text input is active.
        if (!ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                if (audio.playing) {
                    audio_pause(&audio);
                    audio_seek(&audio, audio.play_start);
                } else {
                    if (editor.has_region)
                        audio_seek(&audio, editor.region_start);
                    audio_play(&audio);
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
                audio_seek(&audio, audio_get_position(&audio) - 5.0);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
                audio_seek(&audio, audio_get_position(&audio) + 5.0);

            // Delete / Backspace → selected beats take priority; fall back to
            // removing a selected section only when no beats are selected.
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
                ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                bool any_beats = false;
                for (int i = 0; i < beatmap.count && !any_beats; i++)
                    if (beatmap.beats[i].selected) any_beats = true;

                if (any_beats) {
                    undo_push(&undo, &beatmap);
                    for (int i = beatmap.count - 1; i >= 0; i--)
                        if (beatmap.beats[i].selected)
                            beatmap_remove(&beatmap, i);
                } else if (sectionmap.selected_idx >= 0) {
                    sectionmap_remove(&sectionmap, sectionmap.selected_idx);
                    sectionmap.selected_idx = -1;
                }
            }

            // Ctrl+Z → undo
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
                undo_pop(&undo, &beatmap);

            // Ctrl+S → Save Beatmap (silent overwrite if a path is already known)
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
                if (beatmap.save_path[0] != '\0') {
                    beatmap_save(&beatmap, &sectionmap, beatmap.save_path);
                } else {
                    char suggested[256] = "beatmap.txt";
                    if (audio.loaded)
                        beatmap_suggested_name(audio.filename, suggested, sizeof(suggested));
                    char sp[512] = {};
                    if (platform_save_beatmap_dialog(sp, sizeof(sp), suggested))
                        beatmap_save(&beatmap, &sectionmap, sp);
                }
            }
        }

        // Full-screen dockable main window
        {
            // Use logical window size (not framebuffer size) – ImGui works in
            // logical pixels; on Retina the framebuffer is 2× and would make
            // the window double the screen width.
            int win_w, win_h;
            glfwGetWindowSize(window, &win_w, &win_h);
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2((float)win_w, (float)win_h));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
            ImGui::Begin("BeatmapEditor", nullptr,
                         ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoResize   |
                         ImGuiWindowFlags_NoMove     |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_MenuBar);
            ImGui::PopStyleVar();

            // Menu bar
            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu("File")) {
                    if (ImGui::MenuItem("Open Audio...")) { ui_toolbar_open_dialog(); }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Save Beatmap", "Ctrl+S")) {
                        if (beatmap.save_path[0] != '\0') {
                            beatmap_save(&beatmap, &sectionmap, beatmap.save_path);
                        } else {
                            char suggested[256] = "beatmap.txt";
                            if (audio.loaded)
                                beatmap_suggested_name(audio.filename, suggested, sizeof(suggested));
                            char sp[512] = {};
                            if (platform_save_beatmap_dialog(sp, sizeof(sp), suggested))
                                beatmap_save(&beatmap, &sectionmap, sp);
                        }
                    }
                    if (ImGui::MenuItem("Load Beatmap...")) {
                        char load_path[512] = {};
                        if (platform_open_beatmap_dialog(load_path, sizeof(load_path)))
                            beatmap_load(&beatmap, &sectionmap, load_path);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Quit")) glfwSetWindowShouldClose(window, 1);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("View")) {
                    ImGui::MenuItem("ImGui Demo", nullptr, &show_demo);
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }

            // Toolbar strip
            ui_toolbar_render(&editor, &audio, &beatmap, &undo, &recent, &sectionmap);
            ImGui::Separator();

            // Timeline
            ui_timeline_render(&editor, &audio, &spectro, &beatmap, &undo, &sectionmap);

            ImGui::End();
        }

        if (show_demo) ImGui::ShowDemoWindow(&show_demo);

        // Update window title: "Beatmap Editor — filename [*]"
        {
            char title[600];
            if (audio.loaded) {
                const char* slash = strrchr(audio.filename, '/');
                const char* name  = slash ? slash + 1 : audio.filename;
                snprintf(title, sizeof(title), "Beatmap Editor \xe2\x80\x94 %s%s",
                         name, beatmap.dirty ? " *" : "");
            } else {
                snprintf(title, sizeof(title), "Beatmap Editor%s",
                         beatmap.dirty ? " *" : "");
            }
            glfwSetWindowTitle(window, title);
        }

        // Unsaved-changes quit dialog
        if (show_quit_modal) {
            ImGui::OpenPopup("Unsaved Changes");
            show_quit_modal = false;
        }
        if (ImGui::BeginPopupModal("Unsaved Changes", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("The beatmap has unsaved changes.");
            ImGui::Spacing();
            if (ImGui::Button("Save and Quit", ImVec2(130, 0))) {
                if (beatmap.save_path[0] != '\0') {
                    beatmap_save(&beatmap, &sectionmap, beatmap.save_path);
                } else {
                    char suggested[256] = "beatmap.txt";
                    if (audio.loaded)
                        beatmap_suggested_name(audio.filename, suggested, sizeof(suggested));
                    char sp[512] = {};
                    if (platform_save_beatmap_dialog(sp, sizeof(sp), suggested))
                        beatmap_save(&beatmap, &sectionmap, sp);
                }
                glfwSetWindowShouldClose(window, 1);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard", ImVec2(80, 0))) {
                glfwSetWindowShouldClose(window, 1);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::Render();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.08f, 0.08f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    undo_shutdown(&undo);
    sectionmap_shutdown(&sectionmap);
    beatmap_shutdown(&beatmap);
    audio_shutdown(&audio);
    spectrogram_shutdown(&spectro);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
