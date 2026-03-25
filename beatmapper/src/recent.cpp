#include "recent.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void recent_filepath(char* out, int out_size) {
    const char* home = getenv("HOME");
    if (!home) home = ".";
    snprintf(out, out_size, "%s/.beatmapper", home);
}

void recent_init(RecentFiles* rf) {
    rf->count = 0;
    memset(rf->paths, 0, sizeof(rf->paths));
}

void recent_load(RecentFiles* rf) {
    rf->count = 0;
    char fpath[512];
    recent_filepath(fpath, sizeof(fpath));
    FILE* f = fopen(fpath, "r");
    if (!f) return;
    char line[512];
    while (rf->count < RECENT_MAX && fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len > 0)
            strncpy(rf->paths[rf->count++], line, 511);
    }
    fclose(f);
}

void recent_save(const RecentFiles* rf) {
    char fpath[512];
    recent_filepath(fpath, sizeof(fpath));
    FILE* f = fopen(fpath, "w");
    if (!f) return;
    for (int i = 0; i < rf->count; i++)
        fprintf(f, "%s\n", rf->paths[i]);
    fclose(f);
}

void recent_add(RecentFiles* rf, const char* path) {
    // Remove any existing duplicate
    for (int i = 0; i < rf->count; i++) {
        if (strcmp(rf->paths[i], path) == 0) {
            memmove(&rf->paths[i], &rf->paths[i + 1],
                    (rf->count - i - 1) * sizeof(rf->paths[0]));
            rf->count--;
            break;
        }
    }
    // Shift down and prepend
    int keep = rf->count < RECENT_MAX - 1 ? rf->count : RECENT_MAX - 1;
    memmove(&rf->paths[1], &rf->paths[0], keep * sizeof(rf->paths[0]));
    strncpy(rf->paths[0], path, 511);
    rf->paths[0][511] = '\0';
    rf->count = keep + 1;
}
