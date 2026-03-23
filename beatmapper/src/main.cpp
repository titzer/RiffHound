#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include "audio.h"
#include "spectrogram.h"
#include "editor.h"
#include "ui_timeline.h"
#include "ui_toolbar.h"

#include <stdio.h>
#include <stdlib.h>

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
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

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
    AudioState      audio;
    SpectrogramState spectro;
    EditorState     editor;

    audio_init(&audio);
    spectrogram_init(&spectro);
    editor_init(&editor);

    // If a file was passed on the command line, load it
    if (argc >= 2) {
        audio_load(&audio, argv[1]);
        spectrogram_set_duration(&spectro, audio.duration);
        spectro.computed = true;  // stub: mark as "computed"
        editor.duration  = audio.duration;
        editor.view_end  = (audio.duration < 30.0) ? audio.duration : 30.0;
        editor_clamp_view(&editor);
    }

    static bool show_demo = false;

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Sync position and playing state from the audio thread.
        audio_update(&audio);

        // Sync spectrogram duration when audio is loaded
        if (audio.loaded) {
            spectrogram_set_duration(&spectro, audio.duration);
            spectro.computed = true;
            if (editor.duration != audio.duration) {
                editor.duration = audio.duration;
                editor_clamp_view(&editor);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Space = play / stop (stop rewinds to 0).
        // Checked after NewFrame so key state is fresh.
        // Only blocked when a text input widget is active.
        if (ImGui::IsKeyPressed(ImGuiKey_Space) && !ImGui::IsAnyItemActive()) {
            if (audio.playing) {
                audio_pause(&audio);
                audio_seek(&audio, 0.0);
            } else {
                audio_play(&audio);
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
                    if (ImGui::MenuItem("Open...")) { /* handled in toolbar */ }
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
            ui_toolbar_render(&editor, &audio);
            ImGui::Separator();

            // Timeline
            ui_timeline_render(&editor, &audio, &spectro);

            ImGui::End();
        }

        if (show_demo) ImGui::ShowDemoWindow(&show_demo);

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
    audio_shutdown(&audio);
    spectrogram_shutdown(&spectro);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
