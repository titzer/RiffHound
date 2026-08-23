#include "library.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static const char* AUDIO_EXT[] = { ".mp3", ".wav", ".flac", ".m4a", ".ogg", nullptr };

static void dirs_filepath(char* out, int n) {
    const char* home = getenv("HOME");
    if (!home) home = ".";
    snprintf(out, n, "%s/.beatmapper_dirs", home);
}

void library_init(Library* lib) {
    memset(lib, 0, sizeof(*lib));
}

void library_shutdown(Library* lib) {
    free(lib->entries);
    lib->entries = nullptr;
    lib->count = lib->capacity = 0;
}

static bool is_audio(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    for (int i = 0; AUDIO_EXT[i]; i++) if (strcasecmp(dot, AUDIO_EXT[i]) == 0) return true;
    return false;
}

bool library_read_flags(const char* txt_path, LibFlags* out, int* n_beats) {
    memset(out, 0, sizeof(*out));
    if (n_beats) *n_beats = 0;
    FILE* f = fopen(txt_path, "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        // t_start <tab> t_end <tab> event
        const char* p = line;
        const char* tab1 = strchr(p, '\t'); if (!tab1) continue;
        const char* tab2 = strchr(tab1 + 1, '\t'); if (!tab2) continue;
        const char* ev = tab2 + 1;
        while (*ev == ' ') ev++;
        if (ev[0] == 'B' && (ev[1] == '\n' || ev[1] == '\r' || ev[1] == 0)) { out->beats = true; if (n_beats) (*n_beats)++; }
        else if (!strncmp(ev, "chord:", 6))  out->chords = true;
        else if (!strncmp(ev, "lyric:", 6))  out->lyrics = true;
        else if (!strncmp(ev, "loop:", 5))   out->loops = true;
        else {
            static const char* SECTIONS[] = { "intro", "verse", "pre-chorus", "chorus", "post-chorus",
                "bridge", "breakdown", "instrumental", "solo", "interlude", "outro", "refrain", nullptr };
            for (int i = 0; SECTIONS[i]; i++) {
                size_t n = strlen(SECTIONS[i]);
                if (!strncmp(ev, SECTIONS[i], n) && (ev[n] == ' ' || ev[n] == ':' || ev[n] == '\n' || ev[n] == '\r' || ev[n] == 0 || ev[n] == '@'))
                    out->sections = true;
            }
        }
    }
    fclose(f);
    return true;
}

static LibEntry* find_entry(Library* lib, const char* path) {
    for (int i = 0; i < lib->count; i++)
        if (strcmp(lib->entries[i].path, path) == 0) return &lib->entries[i];
    return nullptr;
}

static LibEntry* add_entry(Library* lib, const char* path, const char* dir) {
    if (lib->count == lib->capacity) {
        int nc = lib->capacity ? lib->capacity * 2 : 64;
        LibEntry* ne = (LibEntry*)realloc(lib->entries, (size_t)nc * sizeof(LibEntry));
        if (!ne) return nullptr;
        lib->entries = ne; lib->capacity = nc;
    }
    LibEntry* e = &lib->entries[lib->count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->path, path, sizeof(e->path) - 1);
    strncpy(e->dir, dir, sizeof(e->dir) - 1);
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    strncpy(e->title, name, sizeof(e->title) - 1);
    char* dot = strrchr(e->title, '.');
    if (dot) *dot = 0;
    return e;
}

// Re-read the sidecar if it changed (or appeared / vanished).
static void refresh_entry(LibEntry* e) {
    char txt[512];
    strncpy(txt, e->path, sizeof(txt) - 1); txt[sizeof(txt) - 1] = 0;
    char* dot = strrchr(txt, '.');
    const char* slash = strrchr(txt, '/');
    if (dot && (!slash || dot > slash)) strcpy(dot, ".txt"); else strncat(txt, ".txt", sizeof(txt) - strlen(txt) - 1);
    struct stat st;
    if (stat(txt, &st) != 0) {
        e->has_txt = false; memset(&e->has, 0, sizeof(e->has)); e->n_beats = 0;
        e->txt_size = e->txt_mtime = 0;
        return;
    }
    if (e->has_txt && e->txt_size == (long)st.st_size && e->txt_mtime == (long)st.st_mtime) return;
    e->has_txt = true;
    e->txt_size = (long)st.st_size; e->txt_mtime = (long)st.st_mtime;
    library_read_flags(txt, &e->has, &e->n_beats);
}

static void scan_dir(Library* lib, const char* top, const char* dir, int depth) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 2) scan_dir(lib, top, full, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_audio(de->d_name)) continue;
        LibEntry* e = find_entry(lib, full);
        if (!e) e = add_entry(lib, full, top);
        if (e) refresh_entry(e);
    }
    closedir(d);
}

void library_rescan(Library* lib) {
    // Drop entries whose folder is no longer remembered or whose file is gone
    int j = 0;
    for (int i = 0; i < lib->count; i++) {
        LibEntry& e = lib->entries[i];
        bool keep = false;
        for (int k = 0; k < lib->dir_count; k++) if (strcmp(e.dir, lib->dirs[k]) == 0) keep = true;
        struct stat st;
        if (keep && stat(e.path, &st) != 0) keep = false;
        if (keep) lib->entries[j++] = e;
    }
    lib->count = j;
    for (int k = 0; k < lib->dir_count; k++) scan_dir(lib, lib->dirs[k], lib->dirs[k], 0);
}

void library_load(Library* lib) {
    char fpath[512];
    dirs_filepath(fpath, sizeof(fpath));
    lib->dir_count = 0;
    FILE* f = fopen(fpath, "r");
    if (f) {
        char line[512];
        while (lib->dir_count < LIB_MAX_DIRS && fgets(line, sizeof(line), f)) {
            int len = (int)strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            if (len > 0) strncpy(lib->dirs[lib->dir_count++], line, 511);
        }
        fclose(f);
    }
    library_rescan(lib);
}

void library_save(const Library* lib) {
    char fpath[512];
    dirs_filepath(fpath, sizeof(fpath));
    FILE* f = fopen(fpath, "w");
    if (!f) return;
    for (int i = 0; i < lib->dir_count; i++) fprintf(f, "%s\n", lib->dirs[i]);
    fclose(f);
}

void library_add_dir(Library* lib, const char* dir) {
    char clean[512];
    strncpy(clean, dir, sizeof(clean) - 1); clean[sizeof(clean) - 1] = 0;
    size_t n = strlen(clean);
    while (n > 1 && clean[n - 1] == '/') clean[--n] = 0;
    for (int i = 0; i < lib->dir_count; i++) {
        if (strcmp(lib->dirs[i], clean) == 0) {
            memmove(&lib->dirs[i], &lib->dirs[i + 1], (size_t)(lib->dir_count - i - 1) * sizeof(lib->dirs[0]));
            lib->dir_count--;
            break;
        }
    }
    if (lib->dir_count == LIB_MAX_DIRS) lib->dir_count--;
    memmove(&lib->dirs[1], &lib->dirs[0], (size_t)lib->dir_count * sizeof(lib->dirs[0]));
    strncpy(lib->dirs[0], clean, 511); lib->dirs[0][511] = 0;
    lib->dir_count++;
    library_save(lib);
    library_rescan(lib);
}

void library_remove_dir(Library* lib, int idx) {
    if (idx < 0 || idx >= lib->dir_count) return;
    memmove(&lib->dirs[idx], &lib->dirs[idx + 1], (size_t)(lib->dir_count - idx - 1) * sizeof(lib->dirs[0]));
    lib->dir_count--;
    library_save(lib);
    library_rescan(lib);
}

void library_touch(Library* lib, const char* path) {
    LibEntry* e = find_entry(lib, path);
    if (e) e->last_opened = (double)time(nullptr);
}

void library_refresh(Library* lib, const char* path) {
    LibEntry* e = find_entry(lib, path);
    if (e) { e->has_txt = false; refresh_entry(e); }
}

bool library_fuzzy(const char* q, const char* s) {
    if (!q || !q[0]) return true;
    int qi = 0;
    for (int i = 0; s[i] && q[qi]; i++) {
        char a = s[i], b = q[qi];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a == b) qi++;
    }
    return q[qi] == 0;
}
