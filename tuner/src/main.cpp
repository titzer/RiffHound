// Tuner: a small always-available window that shows the pitch of whatever
// the microphone hears, against a piano keyboard.  See README.md.

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "audio_io.h"
#include "platform.h"
#include "tuner.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

static void usage() {
    printf("usage: tuner [--selftest] [--no-mic]\n"
           "  --selftest   run the pitch detector on synthetic tones and exit\n"
           "  --no-mic     start with the microphone off\n");
}

// One analysis step: newest window of input -> YIN -> tracker -> history.
static void analyze(Tuner& t, Yin& yin, PitchTracker& tracker, std::vector<float>& buf, double now) {
    if (t.restart_capture) {
        t.restart_capture = false;
        tracker.reset();
        if (t.mic_on) {
            if (!capture_start(t.s.input_device)) t.mic_on = false;
        } else {
            capture_stop();
        }
    }

    YinResult r;
    if (t.mic_on && capture_running()) {
        buf.resize(t.s.window_size);
        if (capture_latest(buf.data(), t.s.window_size)) {
            r = yin.detect(buf.data(), t.s.window_size, capture_sample_rate(),
                           t.s.min_freq, t.s.max_freq, t.s.yin_threshold,
                           powf(10.0f, t.s.gate_db / 20.0f));
        }
    }
    t.raw = r;
    float db = r.rms > 0 ? 20.0f * log10f(r.rms) : -120.0f;
    // Meter ballistics: fast attack, slower release.
    t.level_db = db > t.level_db ? db : t.level_db + (db - t.level_db) * 0.15f;

    TrackerParams tp;
    tp.min_confidence = t.s.min_confidence;
    tp.smoothing      = t.s.smoothing;
    tp.hold_ms        = t.s.hold_ms;
    t.freq = t.mic_on ? tracker.update(r, now, tp) : 0.0f;
    if (t.freq > 0) t.last_freq = t.freq;
    if (!t.mic_on) t.last_freq = 0.0f;

    HistPoint p;
    p.t = now;
    p.midi = 0.0f;
    p.err = 0.0f;
    p.ref_note = t.tone_note;
    if (t.freq > 0) {
        p.midi = freq_to_midi(t.freq, t.s.a4);
        // Uncertainty: recent jitter plus low confidence, as in the prototype.
        float sum = 0, sq = 0;
        int n = 0;
        for (auto it = t.history.rbegin(); it != t.history.rend() && now - it->t < 0.7; ++it) {
            if (it->midi <= 0) continue;
            float c = (it->midi - roundf(it->midi)) * 100.0f;
            sum += c; sq += c * c; n++;
        }
        float jitter = 0;
        if (n >= 4) {
            float mean = sum / n;
            jitter = sqrtf(fmaxf(0.0f, sq / n - mean * mean));
        }
        p.err = fminf(50.0f, 2.0f + jitter + (1.0f - r.confidence) * 30.0f);
    }
    t.history.push_back(p);
    while (!t.history.empty() && now - t.history.front().t > t.s.history_s + 0.5) t.history.pop_front();
    while (t.history.size() > 20000) t.history.pop_front();
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    bool start_mic = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) return pitch_selftest() == 0 ? 0 : 1;
        else if (!strcmp(argv[i], "--no-mic")) start_mic = false;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else { usage(); return 2; }
    }

    Tuner t;
    settings_load(&t.s);

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to init GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_FLOATING, t.s.always_on_top ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);   // shown after it is placed

    GLFWwindow* window = glfwCreateWindow(t.s.win_w, t.s.win_h, "Tuner", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    t.window = window;
    glfwSetWindowSizeLimits(window, 320, 220, GLFW_DONT_CARE, GLFW_DONT_CARE);
    if (t.s.win_x >= 0 && t.s.win_y >= 0) {
        // Only restore a position that is still on some monitor.
        int count = 0;
        GLFWmonitor** mons = glfwGetMonitors(&count);
        for (int i = 0; i < count; i++) {
            int mx, my, mw, mh;
            glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
            if (t.s.win_x >= mx && t.s.win_y >= my && t.s.win_x < mx + mw - 40 && t.s.win_y < my + mh - 40) {
                glfwSetWindowPos(window, t.s.win_x, t.s.win_y);
                break;
            }
        }
    }
    glfwShowWindow(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    platform_set_app_icon();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    static char ini_path[512];
    const char* home = getenv("HOME");
    snprintf(ini_path, sizeof(ini_path), "%s/.tuner_ui.ini", home ? home : ".");
    ImGui::GetIO().IniFilename = ini_path;
    ui_apply_style();
    {
        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(window, &sx, &sy);
        ui_load_fonts(sx > 1.0f ? sx : 1.0f);
    }
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    audio_init();
    t.mic_on = start_mic;
    t.restart_capture = true;

    Yin yin;
    PitchTracker tracker;
    std::vector<float> buf;
    bool floating_applied = t.s.always_on_top;
    bool was_fullscreen = false;
    double last_save = glfwGetTime();

    while (!glfwWindowShouldClose(window) && !t.quit) {
        double frame_t0 = glfwGetTime();
        glfwPollEvents();
        double now = glfwGetTime();

        t.fullscreen = platform_is_fullscreen(window);
        if (t.fullscreen && !was_fullscreen) t.fs_entered_t = now;
        was_fullscreen = t.fullscreen;
        bool want_floating = t.s.always_on_top && !t.fullscreen;
        if (want_floating != floating_applied) {
            glfwSetWindowAttrib(window, GLFW_FLOATING, want_floating ? GLFW_TRUE : GLFW_FALSE);
            floating_applied = want_floating;
        }

        analyze(t, yin, tracker, buf, now);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ui_frame(t, now);
        ImGui::Render();
        if (getenv("TUNER_DEBUG") && ImGui::GetFrameCount() == 2)
            fprintf(stderr, "[tuner] font atlas %dx%d\n", ImGui::GetIO().Fonts->TexWidth, ImGui::GetIO().Fonts->TexHeight);

        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.063f, 0.071f, 0.086f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        // Remember the windowed geometry (not the fullscreen one).
        if (!t.fullscreen && !glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            int x, y, w, h;
            glfwGetWindowPos(window, &x, &y);
            glfwGetWindowSize(window, &w, &h);
            if (x != t.s.win_x || y != t.s.win_y || w != t.s.win_w || h != t.s.win_h) {
                t.s.win_x = x; t.s.win_y = y; t.s.win_w = w; t.s.win_h = h;
                t.settings_dirty = true;
            }
        }
        if (t.settings_dirty && now - last_save > 2.0) {
            settings_save(t.s);
            t.settings_dirty = false;
            last_save = now;
        }

        // Frame pacing.  The window is meant to sit in the background and
        // still be readable, so unfocused it keeps a smooth 30 fps while
        // listening; minimised or idle it drops to a trickle.  The cap also
        // guards against a spinning loop when macOS stops blocking the swap
        // for an occluded window.
        bool focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
        bool hidden  = glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0;
        double min_frame = hidden ? 1.0 / 5.0
                         : focused ? 1.0 / 120.0
                         : t.mic_on || t.tone_note >= 0 ? 1.0 / 30.0
                         : 1.0 / 10.0;
        double spent = glfwGetTime() - frame_t0;
        if (spent < min_frame) usleep((useconds_t)((min_frame - spent) * 1e6));
    }

    tone_set(0.0f, 0, 0.0f);
    settings_save(t.s);
    audio_shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
