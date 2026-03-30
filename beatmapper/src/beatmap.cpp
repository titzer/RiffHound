#include "beatmap.h"
#include "sectionmap.h"
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

bool beatmap_save(BeatMap* bm, SectionMap* sm, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[beatmap] failed to open '%s' for writing\n", path);
        return false;
    }
    fprintf(f, "# Beatmap\n");
    for (int i = 0; i < bm->count; i++)
        fprintf(f, "%.6f\t%.6f\tB\n", bm->beats[i].time, bm->beats[i].time);

    if (sm && sm->count > 0) {
        fprintf(f, "# Sections\n");
        for (int i = 0; i < sm->count; i++) {
            const Section& s = sm->sections[i];
            if (s.label[0])
                fprintf(f, "%.6f\t%.6f\t%s: %s\n",
                        s.t_start, s.t_end, SECTION_KIND_NAMES[s.kind], s.label);
            else
                fprintf(f, "%.6f\t%.6f\t%s\n",
                        s.t_start, s.t_end, SECTION_KIND_NAMES[s.kind]);
        }
        sm->dirty = false;
    }

    fclose(f);
    fprintf(stderr, "[beatmap] saved %d beats + %d sections to '%s'\n",
            bm->count, sm ? sm->count : 0, path);
    beatmap_commit(bm);
    strncpy(bm->save_path, path, sizeof(bm->save_path) - 1);
    bm->save_path[sizeof(bm->save_path) - 1] = '\0';
    bm->dirty = false;
    return true;
}

bool beatmap_load(BeatMap* bm, SectionMap* sm, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[beatmap] failed to open '%s'\n", path);
        return false;
    }
    bm->count = 0;
    if (sm) sectionmap_clear(sm);

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Strip inline comments
        char* comment = strchr(line, '#');
        if (comment) *comment = '\0';

        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r') continue;

        // Read t1, t2, then the first token (kind name)
        double t1, t2;
        char   kind_tok[64];
        int    off = 0;
        if (sscanf(p, "%lf %lf %63s%n", &t1, &t2, kind_tok, &off) < 3) continue;

        if (strcmp(kind_tok, "B") == 0) {
            beatmap_add(bm, t1);
        } else if (strncmp(kind_tok, "Bx", 2) == 0) {
            int n = atoi(kind_tok + 2);
            if (n == 1) {
                beatmap_add(bm, t1);
            } else if (n > 1) {
                for (int i = 0; i < n; i++)
                    beatmap_add(bm, t1 + (t2 - t1) * i / (n - 1));
            }
        } else if (sm) {
            // Strip trailing ':' from kind token (handles "verse:" written without space)
            int klen = (int)strlen(kind_tok);
            if (klen > 0 && kind_tok[klen - 1] == ':') kind_tok[--klen] = '\0';

            // Match against known section kind names
            int kind = -1;
            for (int k = 0; k < SK_COUNT; k++) {
                if (strcmp(kind_tok, SECTION_KIND_NAMES[k]) == 0) { kind = k; break; }
            }
            if (kind >= 0) {
                // Collect optional label: rest of line after the first token
                char label[48] = {};
                const char* rest = p + off;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest == ':') {
                    rest++;
                    while (*rest == ' ' || *rest == '\t') rest++;
                }
                if (*rest && *rest != '\n' && *rest != '\r') {
                    strncpy(label, rest, sizeof(label) - 1);
                    int ll = (int)strlen(label);
                    while (ll > 0 && (label[ll-1] <= ' ')) label[--ll] = '\0';
                }
                sectionmap_add(sm, t1, t2, (SectionKind)kind, label);
            }
        }
    }
    fclose(f);
    fprintf(stderr, "[beatmap] loaded %d beats + %d sections from '%s'\n",
            bm->count, sm ? sm->count : 0, path);
    bm->dirty = false;
    if (sm) sm->dirty = false;
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
