#include "beatmap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void beatmap_init(BeatMap* bm) {
    bm->times    = nullptr;
    bm->count    = 0;
    bm->capacity = 0;
}

void beatmap_shutdown(BeatMap* bm) {
    free(bm->times);
    bm->times    = nullptr;
    bm->count    = 0;
    bm->capacity = 0;
}

int beatmap_add(BeatMap* bm, double t) {
    // Binary search for insert position
    int lo = 0, hi = bm->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (bm->times[mid] < t) lo = mid + 1;
        else                    hi = mid;
    }
    int pos = lo;

    // Reject if within 5 ms of an adjacent beat
    const double DUP_TOL = 0.005;
    if (pos > 0          && t - bm->times[pos-1] < DUP_TOL) return -1;
    if (pos < bm->count  && bm->times[pos] - t   < DUP_TOL) return -1;

    // Grow if needed
    if (bm->count >= bm->capacity) {
        int new_cap = (bm->capacity == 0) ? 64 : bm->capacity * 2;
        double* tmp = (double*)realloc(bm->times, new_cap * sizeof(double));
        if (!tmp) return -1;
        bm->times    = tmp;
        bm->capacity = new_cap;
    }

    // Shift right and insert
    memmove(bm->times + pos + 1, bm->times + pos,
            (bm->count - pos) * sizeof(double));
    bm->times[pos] = t;
    bm->count++;
    return pos;
}

void beatmap_remove(BeatMap* bm, int idx) {
    if (idx < 0 || idx >= bm->count) return;
    memmove(bm->times + idx, bm->times + idx + 1,
            (bm->count - idx - 1) * sizeof(double));
    bm->count--;
}

bool beatmap_save(const BeatMap* bm, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[beatmap] failed to open '%s' for writing\n", path);
        return false;
    }
    fprintf(f, "# Beatmap\n");
    for (int i = 0; i < bm->count; i++)
        fprintf(f, "%.6f\t%.6f\tB\n", bm->times[i], bm->times[i]);
    fclose(f);
    fprintf(stderr, "[beatmap] saved %d beats to '%s'\n", bm->count, path);
    return true;
}

bool beatmap_load(BeatMap* bm, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[beatmap] failed to open '%s'\n", path);
        return false;
    }

    bm->count = 0;  // clear existing beats; keep allocated buffer

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Strip inline comment
        char* comment = strchr(line, '#');
        if (comment) *comment = '\0';

        // Skip blank / whitespace-only lines
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
                for (int i = 0; i < n; i++) {
                    double t = t1 + (t2 - t1) * i / (n - 1);
                    beatmap_add(bm, t);
                }
            }
        }
        // Ignore unknown event types (sections, etc.)
    }

    fclose(f);
    fprintf(stderr, "[beatmap] loaded %d beats from '%s'\n", bm->count, path);
    return true;
}
