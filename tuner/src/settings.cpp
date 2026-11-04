#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Plain "key = value" lines in ~/.tuner_settings; unknown keys are ignored
// so older and newer builds can share the file.

static std::string settings_path() {
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/.tuner_settings";
}

template <typename T> static T clampv(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }

void settings_clamp(Settings* s) {
    s->a4             = clampv(s->a4, 400.0f, 480.0f);
    s->yin_threshold  = clampv(s->yin_threshold, 0.02f, 0.5f);
    s->min_confidence = clampv(s->min_confidence, 0.0f, 0.95f);
    s->smoothing      = clampv(s->smoothing, 0.0f, 0.95f);
    s->min_freq       = clampv(s->min_freq, 25.0f, 400.0f);
    s->max_freq       = clampv(s->max_freq, s->min_freq * 2.0f, 4000.0f);
    if (s->window_size != 2048 && s->window_size != 4096 && s->window_size != 8192) s->window_size = 4096;
    s->hold_ms        = clampv(s->hold_ms, 0.0f, 2000.0f);
    s->gate_db        = clampv(s->gate_db, -90.0f, -10.0f);
    s->in_tune_cents  = clampv(s->in_tune_cents, 1.0f, 25.0f);
    s->close_cents    = clampv(s->close_cents, s->in_tune_cents, 50.0f);
    s->history_s      = clampv(s->history_s, 2.0f, 60.0f);
    s->octave_shift   = clampv(s->octave_shift, -2, 3);
    s->tone_wave      = clampv(s->tone_wave, 0, 3);
    s->tone_volume    = clampv(s->tone_volume, 0.0f, 1.0f);
    s->win_w          = clampv(s->win_w, 320, 10000);
    s->win_h          = clampv(s->win_h, 220, 10000);
}

void settings_load(Settings* s) {
    FILE* f = fopen(settings_path().c_str(), "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = line;
        char* val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char* e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = '\0';
        while (*val == ' ' || *val == '\t') val++;
        size_t vl = strlen(val);
        while (vl && (val[vl - 1] == '\n' || val[vl - 1] == '\r')) val[--vl] = '\0';

        float fv = (float)atof(val);
        int   iv = atoi(val);
#define F(name) else if (!strcmp(key, #name)) s->name = fv;
#define I(name) else if (!strcmp(key, #name)) s->name = iv;
#define B(name) else if (!strcmp(key, #name)) s->name = iv != 0;
        if (0) {}
        F(a4) F(yin_threshold) F(min_confidence) F(smoothing) F(min_freq) F(max_freq)
        I(window_size) F(hold_ms) F(gate_db) F(in_tune_cents) F(close_cents) F(history_s)
        B(show_history) B(show_key_labels) B(always_on_top) I(octave_shift)
        I(tone_wave) F(tone_volume) I(win_x) I(win_y) I(win_w) I(win_h)
        else if (!strcmp(key, "input_device")) s->input_device = val;
#undef F
#undef I
#undef B
    }
    fclose(f);
    settings_clamp(s);
}

void settings_save(const Settings& s) {
    std::string path = settings_path();
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f, "# tuner settings\n");
#define F(name) fprintf(f, "%s = %g\n", #name, (double)s.name);
#define I(name) fprintf(f, "%s = %d\n", #name, (int)s.name);
    F(a4) F(yin_threshold) F(min_confidence) F(smoothing) F(min_freq) F(max_freq)
    I(window_size) F(hold_ms) F(gate_db) F(in_tune_cents) F(close_cents) F(history_s)
    I(show_history) I(show_key_labels) I(always_on_top) I(octave_shift)
    I(tone_wave) F(tone_volume) I(win_x) I(win_y) I(win_w) I(win_h)
#undef F
#undef I
    fprintf(f, "input_device = %s\n", s.input_device.c_str());
    fclose(f);
    rename(tmp.c_str(), path.c_str());
}
