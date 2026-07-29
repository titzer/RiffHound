#include "timeseries.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void ts_init(TsFile* f) {
    f->events   = nullptr;
    f->count    = 0;
    f->capacity = 0;
}

void ts_shutdown(TsFile* f) {
    free(f->events);
    ts_init(f);
}

static bool ts_grow(TsFile* f) {
    if (f->count < f->capacity) return true;
    int cap = f->capacity ? f->capacity * 2 : 256;
    TsEvent* e = (TsEvent*)realloc(f->events, cap * sizeof(TsEvent));
    if (!e) return false;
    f->events   = e;
    f->capacity = cap;
    return true;
}

int ts_add(TsFile* f, double t_start, double t_end, const char* name,
           const char* comment) {
    if (!ts_grow(f)) return -1;
    TsEvent* e = &f->events[f->count];
    e->t_start = t_start;
    e->t_end   = t_end;
    snprintf(e->name, sizeof(e->name), "%s", name ? name : "");
    snprintf(e->comment, sizeof(e->comment), "%s", comment ? comment : "");
    e->beat_start = -1;
    e->beat_end   = -1;
    return f->count++;
}

// Parse one time token.  Accepts "12.5" or "B37"; sets *beat to the beat
// number for the latter and leaves *t untouched (the caller resolves it).
static bool parse_time(const char* tok, double* t, int* beat) {
    *beat = -1;
    if (tok[0] == 'B' || tok[0] == 'b') {
        char* end = nullptr;
        long n = strtol(tok + 1, &end, 10);
        if (end != tok + 1 && n > 0) { *beat = (int)n; *t = 0.0; return true; }
        return false;
    }
    char* end = nullptr;
    double v = strtod(tok, &end);
    if (end == tok) return false;
    *t = v;
    return true;
}

bool ts_load(TsFile* f, const char* path) {
    f->count = 0;
    FILE* fp = fopen(path, "r");
    if (!fp) return false;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        // Split off the trailing comment; it carries provenance (~src, ~conf).
        char comment[128] = {};
        char* hash = strchr(line, '#');
        if (hash) {
            const char* c = hash + 1;
            while (*c == ' ' || *c == '\t') c++;
            snprintf(comment, sizeof(comment), "%s", c);
            int n = (int)strlen(comment);
            while (n > 0 && comment[n - 1] <= ' ') comment[--n] = '\0';
            *hash = '\0';
        }

        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r') continue;

        // Three whitespace-separated fields: start, end, then the rest.
        char tok1[64], tok2[64];
        int off = 0;
        if (sscanf(p, "%63s %63s%n", tok1, tok2, &off) < 2) continue;

        double t1 = 0, t2 = 0;
        int b1 = -1, b2 = -1;
        if (!parse_time(tok1, &t1, &b1)) continue;
        if (!parse_time(tok2, &t2, &b2)) continue;

        const char* rest = p + off;
        while (*rest == ' ' || *rest == '\t') rest++;
        char name[256] = {};
        snprintf(name, sizeof(name), "%s", rest);
        int n = (int)strlen(name);
        while (n > 0 && name[n - 1] <= ' ') name[--n] = '\0';
        if (!name[0]) continue;

        // Expand BxN runs the way beatmap_load does.
        if (name[0] == 'B' && name[1] == 'x') {
            int cnt = atoi(name + 2);
            if (cnt == 1) {
                int i = ts_add(f, t1, t1, "B", comment);
                if (i >= 0) { f->events[i].beat_start = b1; f->events[i].beat_end = b1; }
            } else if (cnt > 1) {
                for (int k = 0; k < cnt; k++) {
                    double t = t1 + (t2 - t1) * k / (cnt - 1);
                    ts_add(f, t, t, "B", k == 0 ? comment : "");
                }
            }
            continue;
        }

        int i = ts_add(f, t1, t2, name, comment);
        if (i >= 0) { f->events[i].beat_start = b1; f->events[i].beat_end = b2; }
    }
    fclose(fp);
    return true;
}

void ts_write(const TsFile* f, FILE* out) {
    for (int i = 0; i < f->count; i++) {
        const TsEvent* e = &f->events[i];
        char a[32], b[32];
        if (e->beat_start > 0) snprintf(a, sizeof(a), "B%d", e->beat_start);
        else                   snprintf(a, sizeof(a), "%.6f", e->t_start);
        if (e->beat_end > 0)   snprintf(b, sizeof(b), "B%d", e->beat_end);
        else                   snprintf(b, sizeof(b), "%.6f", e->t_end);

        if (e->comment[0]) fprintf(out, "%s\t%s\t%s\t# %s\n", a, b, e->name, e->comment);
        else               fprintf(out, "%s\t%s\t%s\n", a, b, e->name);
    }
}

bool ts_save(const TsFile* f, const char* path) {
    FILE* fp = fopen(path, "w");
    if (!fp) return false;
    ts_write(f, fp);
    fclose(fp);
    return true;
}

static int ts_cmp(const void* a, const void* b) {
    const TsEvent* x = (const TsEvent*)a;
    const TsEvent* y = (const TsEvent*)b;
    if (x->t_start < y->t_start) return -1;
    if (x->t_start > y->t_start) return  1;
    if (x->t_end   < y->t_end)   return -1;
    if (x->t_end   > y->t_end)   return  1;
    return 0;
}

void ts_sort(TsFile* f) {
    if (f->count > 1) qsort(f->events, f->count, sizeof(TsEvent), ts_cmp);
}

int ts_beat_times(const TsFile* f, double* out, int max) {
    int n = 0;
    for (int i = 0; i < f->count && n < max; i++) {
        if (strcmp(f->events[i].name, "B") == 0) out[n++] = f->events[i].t_start;
    }
    // The caller may have handed us an unsorted file.
    for (int i = 1; i < n; i++) {
        double v = out[i];
        int j = i - 1;
        while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return n;
}

bool ts_is(const TsEvent* e, const char* prefix) {
    size_t n = strlen(prefix);
    if (strncmp(e->name, prefix, n) != 0) return false;
    char c = e->name[n];
    return c == '\0' || c == ' ' || c == '\t' || c == ':';
}

const char* ts_payload(const TsEvent* e) {
    const char* c = strchr(e->name, ':');
    if (!c) return "";
    c++;
    while (*c == ' ' || *c == '\t') c++;
    return c;
}
