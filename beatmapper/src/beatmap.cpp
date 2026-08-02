#include "beatmap.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "miscmap.h"
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

bool beatmap_save(BeatMap* bm, SectionMap* sm, LyricMap* lm, MiscMap* mm, const char* path) {
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
                fprintf(f, "%.6f\t%.6f\t%s: %s @%d/%d\n",
                        s.t_start, s.t_end, SECTION_KIND_NAMES[s.kind],
                        s.label, s.ts_num, s.ts_den);
            else
                fprintf(f, "%.6f\t%.6f\t%s @%d/%d\n",
                        s.t_start, s.t_end, SECTION_KIND_NAMES[s.kind],
                        s.ts_num, s.ts_den);
        }
        sm->dirty = false;
    }

    if (lm && lm->count > 0) {
        fprintf(f, "# Lyrics\n");
        for (int i = 0; i < lm->count; i++) {
            const Lyric& ly = lm->lyrics[i];
            fprintf(f, "%.6f\t%.6f\tlyric: %s\n", ly.t_start, ly.t_end, ly.text);
        }
        lm->dirty = false;
    }

    if (mm && mm->count > 0) {
        fprintf(f, "# Misc\n");
        for (int i = 0; i < mm->count; i++)
            fprintf(f, "%.6f\t%.6f\t%s\n",
                    mm->entries[i].t_start, mm->entries[i].t_end, mm->entries[i].text);
        mm->dirty = false;
    }

    fclose(f);
    fprintf(stderr, "[beatmap] saved %d beats + %d sections + %d lyrics + %d misc to '%s'\n",
            bm->count, sm ? sm->count : 0, lm ? lm->count : 0, mm ? mm->count : 0, path);
    beatmap_commit(bm);
    strncpy(bm->save_path, path, sizeof(bm->save_path) - 1);
    bm->save_path[sizeof(bm->save_path) - 1] = '\0';
    bm->dirty = false;
    return true;
}

bool beatmap_load(BeatMap* bm, SectionMap* sm, LyricMap* lm, MiscMap* mm, const char* path) {
    // Always clear first so stale data never persists when the file is missing.
    bm->count = 0;
    if (sm) sectionmap_clear(sm);
    if (lm) lyricmap_clear(lm);
    if (mm) miscmap_clear(mm);

    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[beatmap] failed to open '%s'\n", path);
        return false;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Strip inline comments.  A '#' only starts one at the start of a line
        // or after whitespace: cutting at the first '#' unconditionally eats
        // every sharp, turning "chord: F#m7" into "chord: F" with nothing
        // downstream able to tell.  Same rule as lib/riffdsp/timeseries.cpp.
        for (char* c = line; *c; c++) {
            if (*c == '#' && (c == line || c[-1] == ' ' || c[-1] == '\t')) {
                *c = '\0';
                break;
            }
        }

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
        } else if (lm && (strcmp(kind_tok, "lyric:") == 0 ||
                          strcmp(kind_tok, "lyric")  == 0)) {
            const char* rest = p + off;
            while (*rest == ' ' || *rest == '\t') rest++;
            if (*rest == ':') { rest++; while (*rest == ' ' || *rest == '\t') rest++; }
            char text[128] = {};
            strncpy(text, rest, sizeof(text) - 1);
            int ll = (int)strlen(text);
            while (ll > 0 && (text[ll - 1] <= ' ')) text[--ll] = '\0';
            lyricmap_add(lm, t1, t2, text);
        } else if (strncmp(kind_tok, "Bx", 2) == 0) {
            int n = atoi(kind_tok + 2);
            if (n == 1) {
                beatmap_add(bm, t1);
            } else if (n > 1) {
                for (int i = 0; i < n; i++)
                    beatmap_add(bm, t1 + (t2 - t1) * i / (n - 1));
            }
        } else {
            // Save original token before stripping ':' (needed for misc round-trip)
            char orig_tok[64];
            strncpy(orig_tok, kind_tok, sizeof(orig_tok) - 1);
            orig_tok[sizeof(orig_tok) - 1] = '\0';

            // Strip trailing ':' from kind token (handles "verse:" written without space)
            int klen = (int)strlen(kind_tok);
            if (klen > 0 && kind_tok[klen - 1] == ':') kind_tok[--klen] = '\0';

            // Match against known section kind names
            int kind = -1;
            for (int k = 0; k < SK_COUNT; k++) {
                if (strcmp(kind_tok, SECTION_KIND_NAMES[k]) == 0) { kind = k; break; }
            }
            if (kind >= 0 && sm) {
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
                // Parse optional @N/D time-signature suffix from end of label
                int ts_num = 4, ts_den = 4;
                {
                    int ll = (int)strlen(label);
                    for (int k = ll - 1; k >= 0; k--) {
                        if (label[k] == '@') {
                            int n = 0, d = 0, rd = 0;
                            if (sscanf(label + k + 1, "%d/%d%n", &n, &d, &rd) == 2
                                    && n > 0 && d > 0) {
                                ts_num = n;
                                ts_den = d;
                                // Trim trailing whitespace before '@'
                                while (k > 0 && (label[k-1]==' ' || label[k-1]=='\t')) k--;
                                label[k] = '\0';
                            }
                            break;
                        }
                    }
                }
                int sec_idx = sectionmap_add(sm, t1, t2, (SectionKind)kind, label);
                if (sec_idx >= 0) {
                    sm->sections[sec_idx].ts_num = ts_num;
                    sm->sections[sec_idx].ts_den = ts_den;
                }
            } else if (mm) {
                // Unrecognized line: store as misc annotation preserving original text
                char misc_text[128] = {};
                const char* rest = p + off;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest && *rest != '\n' && *rest != '\r')
                    snprintf(misc_text, sizeof(misc_text), "%s %s", orig_tok, rest);
                else
                    strncpy(misc_text, orig_tok, sizeof(misc_text) - 1);
                int ll = (int)strlen(misc_text);
                while (ll > 0 && misc_text[ll-1] <= ' ') misc_text[--ll] = '\0';
                miscmap_add(mm, t1, t2, misc_text);
            }
        }
    }
    fclose(f);
    fprintf(stderr, "[beatmap] loaded %d beats + %d sections + %d lyrics + %d misc from '%s'\n",
            bm->count, sm ? sm->count : 0, lm ? lm->count : 0, mm ? mm->count : 0, path);
    bm->dirty = false;
    if (sm) sm->dirty = false;
    if (lm) lm->dirty = false;
    if (mm) mm->dirty = false;
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

// --- beat coordinates ----------------------------------------------------

double beatmap_beat_pos(const BeatMap* bm, double t) {
    if (bm->count < 2) return 0.0;
    int lo = 0, hi = bm->count;
    while (lo < hi) { int m = (lo + hi) / 2; if (bm->beats[m].time < t) lo = m + 1; else hi = m; }
    int i = lo - 1;
    if (i < 0)             i = 0;
    if (i > bm->count - 2) i = bm->count - 2;
    double d = bm->beats[i+1].time - bm->beats[i].time;
    if (d <= 0.0) return (double)i;
    return (double)i + (t - bm->beats[i].time) / d;
}

double beatmap_time_at(const BeatMap* bm, double beat) {
    if (bm->count < 2) return 0.0;
    int i = (int)floor(beat);
    if (i < 0)             i = 0;
    if (i > bm->count - 2) i = bm->count - 2;
    double d = bm->beats[i+1].time - bm->beats[i].time;
    return bm->beats[i].time + (beat - (double)i) * d;
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

int beatmap_selection_range(const BeatMap* bm, int* i0, int* i1) {
    int lo = -1, hi = -1, n = 0;
    for (int i = 0; i < bm->count; i++) {
        if (!bm->beats[i].selected) continue;
        if (lo < 0) lo = i;
        hi = i;
        n++;
    }
    if (i0) *i0 = lo;
    if (i1) *i1 = hi;
    return n;
}

// --- tempo statistics ----------------------------------------------------

BpmStats beatmap_bpm_stats(const double* times, int n) {
    BpmStats st = { 0, 0.0, 0.0, 0.0, 0.0 };
    if (!times || n < 2) return st;

    double sum = 0.0, sum_sq = 0.0;
    for (int i = 1; i < n; i++) {
        double dt = times[i] - times[i - 1];
        if (dt <= 1e-9) continue;
        double bpm = 60.0 / dt;
        if (st.n == 0) { st.min_bpm = st.max_bpm = bpm; }
        else {
            if (bpm < st.min_bpm) st.min_bpm = bpm;
            if (bpm > st.max_bpm) st.max_bpm = bpm;
        }
        sum    += bpm;
        sum_sq += bpm * bpm;
        st.n++;
    }
    if (st.n > 0) {
        st.mean = sum / st.n;
        double var = sum_sq / st.n - st.mean * st.mean;
        st.stddev = (var > 0.0) ? sqrt(var) : 0.0;
    }
    return st;
}

// --- smoothing -----------------------------------------------------------

void smooth_params_defaults(SmoothParams* p) {
    if (!p) return;
    p->strength     = 0.5f;
    p->iterations   = 20;
    p->use_onsets   = false;
    p->onset_weight = 0.35f;
    p->onset_window = 0.20f;
    p->max_shift    = 0.050f;   // 50 ms
}

// Nearest entry in a chronological array; returns -1 when the array is empty.
static int nearest_time_idx(const double* onsets, int count, double t) {
    if (!onsets || count <= 0) return -1;
    int lo = 0, hi = count;
    while (lo < hi) { int m = (lo + hi) / 2; if (onsets[m] < t) lo = m + 1; else hi = m; }
    int best = -1;
    double bd = 0.0;
    if (lo < count)            { best = lo;     bd = onsets[lo] - t; }
    if (lo > 0) {
        double d = t - onsets[lo - 1];
        if (best < 0 || d < bd) { best = lo - 1; bd = d; }
    }
    return best;
}

void beat_smooth_times(double* t, int n, const SmoothParams* p,
                       const double* onsets, int onset_count)
{
    if (!t || !p || n < 3) return;

    int   iters = (p->iterations < 1) ? 1 : p->iterations;
    float alpha = p->strength;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    // Originals, for the max-shift leash.
    double* orig = (double*)malloc((size_t)n * sizeof(double));
    if (!orig) return;
    memcpy(orig, t, (size_t)n * sizeof(double));

    // Ordering guard: never let two beats come closer than a fraction of the
    // mean spacing (5 ms at most), so a crowded range can still be smoothed.
    double span    = t[n - 1] - t[0];
    double min_gap = (span > 0.0) ? span / (n - 1) * 0.10 : 0.0;
    if (min_gap > 0.005) min_gap = 0.005;

    bool use_onsets = p->use_onsets && onsets && onset_count > 0 &&
                      p->onset_weight > 0.0f && p->onset_window > 0.0f;

    for (int it = 0; it < iters; it++) {
        // Anchors stay put; interior beats move toward their neighbours'
        // midpoint.  Updates are applied in place (Gauss-Seidel) with the sweep
        // direction alternating each pass: in place converges roughly twice as
        // fast as a simultaneous update and does not oscillate at strength 1.0,
        // and alternating removes the directional bias of a single sweep.
        bool forward = ((it & 1) == 0);
        int  k0      = forward ? 1     : n - 2;
        int  k1      = forward ? n - 1 : 0;
        int  step    = forward ? 1     : -1;

        for (int k = k0; k != k1; k += step) {
            double mid = 0.5 * (t[k - 1] + t[k + 1]);
            double v   = t[k] + alpha * (mid - t[k]);

            // Audio guidance: pull toward the nearest detected onset.
            if (use_onsets) {
                double win = 0.5 * (t[k + 1] - t[k - 1]) * p->onset_window;
                if (win > 0.0) {
                    int j = nearest_time_idx(onsets, onset_count, v);
                    if (j >= 0) {
                        double d = onsets[j] - v;
                        if (d <= win && d >= -win) v += p->onset_weight * d;
                    }
                }
            }

            // Leash: keep every beat within max_shift of where it started.
            if (p->max_shift > 0.0f) {
                double d = v - orig[k];
                if (d >  p->max_shift) v = orig[k] + p->max_shift;
                if (d < -p->max_shift) v = orig[k] - p->max_shift;
            }

            // Strict ordering: stay clear of both neighbours.
            double lo = t[k - 1] + min_gap;
            double hi = t[k + 1] - min_gap;
            if (lo <= hi) {
                if (v < lo) v = lo;
                if (v > hi) v = hi;
            } else {
                v = 0.5 * (t[k - 1] + t[k + 1]);   // no room: sit in the middle
            }
            t[k] = v;
        }
    }

    free(orig);
}

void beatmap_apply_times(BeatMap* bm, int i0, const double* times, int n) {
    if (!bm || !times || i0 < 0 || n <= 0 || i0 + n > bm->count) return;
    for (int k = 0; k < n; k++)
        bm->beats[i0 + k].time = times[k];
    bm->dirty = true;
}

// Map t through the old -> new beat table when it sits on a beat.
// Returns false when t was not pinned to any of the moved beats.
static bool retime_one(double* t, const double* old_times, const double* new_times,
                       int n, double tol)
{
    int k = nearest_time_idx(old_times, n, *t);   // same nearest-value search
    if (k < 0) return false;
    if (fabs(old_times[k] - *t) > tol) return false;
    if (fabs(new_times[k] - *t) < 1e-12) return false;   // beat did not move
    *t = new_times[k];
    return true;
}

int beatmap_retime_annotations(SectionMap* sm, LyricMap* lm, MiscMap* mm,
                               const double* old_times, const double* new_times,
                               int n, double tol)
{
    if (!old_times || !new_times || n <= 0) return 0;
    int changed = 0;

    if (sm) {
        bool any = false;
        for (int i = 0; i < sm->count; i++) {
            if (retime_one(&sm->sections[i].t_start, old_times, new_times, n, tol)) { changed++; any = true; }
            if (retime_one(&sm->sections[i].t_end,   old_times, new_times, n, tol)) { changed++; any = true; }
        }
        if (any) sm->dirty = true;
    }
    if (lm) {
        bool any = false;
        for (int i = 0; i < lm->count; i++) {
            if (retime_one(&lm->lyrics[i].t_start, old_times, new_times, n, tol)) { changed++; any = true; }
            if (retime_one(&lm->lyrics[i].t_end,   old_times, new_times, n, tol)) { changed++; any = true; }
        }
        if (any) lm->dirty = true;
    }
    if (mm) {
        bool any = false;
        for (int i = 0; i < mm->count; i++) {
            if (retime_one(&mm->entries[i].t_start, old_times, new_times, n, tol)) { changed++; any = true; }
            if (retime_one(&mm->entries[i].t_end,   old_times, new_times, n, tol)) { changed++; any = true; }
        }
        if (any) mm->dirty = true;
    }
    return changed;
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
