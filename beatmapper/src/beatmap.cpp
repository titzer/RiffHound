#include "beatmap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

void beatmap_init(BeatMap* bm) {
    bm->beats        = nullptr;
    bm->count        = 0;
    bm->capacity     = 0;
    bm->dirty        = false;
    bm->save_path[0] = '\0';
}

void beatmap_shutdown(BeatMap* bm) {
    free(bm->beats);
    bm->beats    = nullptr;
    bm->count    = 0;
    bm->capacity = 0;
}

int beatmap_add(BeatMap* bm, double t) {
    // Binary search for insert position
    int lo = 0, hi = bm->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (bm->beats[mid].time < t) lo = mid + 1;
        else                         hi = mid;
    }
    int pos = lo;

    // Reject if within 5 ms of an adjacent beat
    const double DUP_TOL = 0.005;
    if (pos > 0         && t - bm->beats[pos-1].time < DUP_TOL) return -1;
    if (pos < bm->count && bm->beats[pos].time - t   < DUP_TOL) return -1;

    // Grow if needed
    if (bm->count >= bm->capacity) {
        int new_cap = (bm->capacity == 0) ? 64 : bm->capacity * 2;
        Beat* tmp = (Beat*)realloc(bm->beats, new_cap * sizeof(Beat));
        if (!tmp) return -1;
        bm->beats    = tmp;
        bm->capacity = new_cap;
    }

    // Shift right and insert
    memmove(bm->beats + pos + 1, bm->beats + pos,
            (bm->count - pos) * sizeof(Beat));
    bm->beats[pos] = { t, false, false };
    bm->count++;
    bm->dirty = true;
    return pos;
}

void beatmap_remove(BeatMap* bm, int idx) {
    if (idx < 0 || idx >= bm->count) return;
    memmove(bm->beats + idx, bm->beats + idx + 1,
            (bm->count - idx - 1) * sizeof(Beat));
    bm->count--;
    bm->dirty = true;
}

bool beatmap_save(BeatMap* bm, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[beatmap] failed to open '%s' for writing\n", path);
        return false;
    }
    fprintf(f, "# Beatmap\n");
    for (int i = 0; i < bm->count; i++)
        fprintf(f, "%.6f\t%.6f\tB\n", bm->beats[i].time, bm->beats[i].time);
    fclose(f);
    fprintf(stderr, "[beatmap] saved %d beats to '%s'\n", bm->count, path);
    beatmap_commit(bm);
    strncpy(bm->save_path, path, sizeof(bm->save_path) - 1);
    bm->save_path[sizeof(bm->save_path) - 1] = '\0';
    bm->dirty = false;
    return true;
}

bool beatmap_load(BeatMap* bm, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[beatmap] failed to open '%s'\n", path);
        return false;
    }
    bm->count = 0;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* comment = strchr(line, '#');
        if (comment) *comment = '\0';

        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r') continue;

        double t1, t2;
        char   name[64];
        if (sscanf(p, "%lf %lf %63s", &t1, &t2, name) != 3) continue;

        if (strcmp(name, "B") == 0) {
            beatmap_add(bm, t1);
        } else if (strncmp(name, "Bx", 2) == 0) {
            int n = atoi(name + 2);
            if (n == 1) {
                beatmap_add(bm, t1);
            } else if (n > 1) {
                for (int i = 0; i < n; i++)
                    beatmap_add(bm, t1 + (t2 - t1) * i / (n - 1));
            }
        }
    }
    fclose(f);
    // beatmap_add sets interp=false, so all loaded beats are already fixed
    fprintf(stderr, "[beatmap] loaded %d beats from '%s'\n", bm->count, path);
    bm->dirty = false;  // loaded state matches disk
    return true;
}

void beatmap_path_for_audio(const char* audio_path, char* out, int out_size) {
    strncpy(out, audio_path, out_size - 1);
    out[out_size - 1] = '\0';
    // Locate the extension dot within the filename portion (after the last slash)
    char* name = strrchr(out, '/');
    name = name ? name + 1 : out;
    char* dot = strrchr(name, '.');
    if (dot)
        strcpy(dot, ".txt");
    else if ((int)(strlen(out) + 4) < out_size)
        strcat(out, ".txt");
}

void beatmap_commit(BeatMap* bm) {
    for (int i = 0; i < bm->count; i++)
        bm->beats[i].interp = false;
}

// --- selection helpers ---------------------------------------------------

void beatmap_clear_selection(BeatMap* bm) {
    for (int i = 0; i < bm->count; i++)
        bm->beats[i].selected = false;
}

int beatmap_selected_count(const BeatMap* bm) {
    int n = 0;
    for (int i = 0; i < bm->count; i++)
        if (bm->beats[i].selected) n++;
    return n;
}

double beatmap_selected_bpm(const BeatMap* bm) {
    double t_first = 0.0, t_last = 0.0;
    int n = 0;
    for (int i = 0; i < bm->count; i++) {
        if (!bm->beats[i].selected) continue;
        if (n == 0) t_first = bm->beats[i].time;
        t_last = bm->beats[i].time;
        n++;
    }
    if (n < 2 || t_last <= t_first) return 0.0;
    return 60.0 * (n - 1) / (t_last - t_first);
}

// --- interpolation -------------------------------------------------------

void beatmap_fill(BeatMap* bm, double t1, double t2, double bpm) {
    if (bpm <= 0.0 || t2 <= t1) return;
    int n = (int)round((t2 - t1) * bpm / 60.0);
    if (n <= 1) return;
    for (int k = 1; k < n; k++) {
        int idx = beatmap_add(bm, t1 + (t2 - t1) * k / n);
        if (idx >= 0) bm->beats[idx].interp = true;
    }
}
