#include "sectionmap.h"
#include <stdlib.h>
#include <string.h>

const char* const SECTION_KIND_NAMES[SK_COUNT] = {
    "intro", "verse", "pre-chorus", "chorus", "post-chorus",
    "bridge", "breakdown", "instrumental", "solo",
    "interlude", "outro", "refrain"
};

void sectionmap_init(SectionMap* sm) {
    sm->sections     = nullptr;
    sm->count        = 0;
    sm->capacity     = 0;
    sm->dirty        = false;
    sm->selected_idx = -1;
}

void sectionmap_shutdown(SectionMap* sm) {
    free(sm->sections);
    sm->sections = nullptr;
    sm->count = sm->capacity = 0;
}

void sectionmap_clear(SectionMap* sm) {
    sm->count        = 0;
    sm->dirty        = false;
    sm->selected_idx = -1;
}

int sectionmap_add(SectionMap* sm, double t_start, double t_end,
                   SectionKind kind, const char* label)
{
    if (t_end < t_start) { double tmp = t_start; t_start = t_end; t_end = tmp; }

    // Sorted insertion by t_start
    int pos = sm->count;
    for (int i = 0; i < sm->count; i++) {
        if (sm->sections[i].t_start > t_start) { pos = i; break; }
    }

    if (sm->count >= sm->capacity) {
        int nc = (sm->capacity == 0) ? 16 : sm->capacity * 2;
        Section* tmp = (Section*)realloc(sm->sections, nc * sizeof(Section));
        if (!tmp) return -1;
        sm->sections = tmp;
        sm->capacity = nc;
    }

    memmove(sm->sections + pos + 1, sm->sections + pos,
            (sm->count - pos) * sizeof(Section));

    Section& s = sm->sections[pos];
    s.t_start  = t_start;
    s.t_end    = t_end;
    s.kind     = kind;
    s.ts_num   = 4;
    s.ts_den   = 4;
    s.label[0] = '\0';
    if (label && label[0])
        strncpy(s.label, label, sizeof(s.label) - 1);
    sm->count++;
    sm->dirty = true;

    // Keep selected_idx valid after insertion
    if (sm->selected_idx >= pos) sm->selected_idx++;
    return pos;
}

void sectionmap_remove(SectionMap* sm, int idx) {
    if (idx < 0 || idx >= sm->count) return;
    memmove(sm->sections + idx, sm->sections + idx + 1,
            (sm->count - idx - 1) * sizeof(Section));
    sm->count--;
    sm->dirty = true;
    if (sm->selected_idx == idx)      sm->selected_idx = -1;
    else if (sm->selected_idx > idx)  sm->selected_idx--;
}
