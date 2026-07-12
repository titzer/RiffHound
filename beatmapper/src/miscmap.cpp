#include "miscmap.h"
#include <stdlib.h>
#include <string.h>

void miscmap_init(MiscMap* mm) {
    mm->entries      = nullptr;
    mm->count        = 0;
    mm->capacity     = 0;
    mm->dirty        = false;
    mm->selected_idx = -1;
}

void miscmap_shutdown(MiscMap* mm) {
    free(mm->entries);
    mm->entries  = nullptr;
    mm->count    = 0;
    mm->capacity = 0;
}

void miscmap_clear(MiscMap* mm) {
    mm->count        = 0;
    mm->dirty        = false;
    mm->selected_idx = -1;
}

int miscmap_add(MiscMap* mm, double t_start, double t_end, const char* text) {
    if (t_end < t_start) { double tmp = t_start; t_start = t_end; t_end = tmp; }

    if (mm->count >= mm->capacity) {
        int new_cap = (mm->capacity == 0) ? 16 : mm->capacity * 2;
        MiscAnnotation* tmp = (MiscAnnotation*)realloc(mm->entries,
                                                        new_cap * sizeof(MiscAnnotation));
        if (!tmp) return -1;
        mm->entries  = tmp;
        mm->capacity = new_cap;
    }

    // Sorted insertion by t_start
    int pos = mm->count;
    for (int i = 0; i < mm->count; i++) {
        if (mm->entries[i].t_start > t_start) { pos = i; break; }
    }

    memmove(mm->entries + pos + 1, mm->entries + pos,
            (mm->count - pos) * sizeof(MiscAnnotation));

    MiscAnnotation& ma = mm->entries[pos];
    ma.t_start = t_start;
    ma.t_end   = t_end;
    ma.text[0] = '\0';
    if (text && text[0])
        strncpy(ma.text, text, sizeof(ma.text) - 1);
    mm->count++;
    mm->dirty = true;

    if (mm->selected_idx >= pos) mm->selected_idx++;
    return pos;
}

void miscmap_remove(MiscMap* mm, int idx) {
    if (idx < 0 || idx >= mm->count) return;
    memmove(mm->entries + idx, mm->entries + idx + 1,
            (mm->count - idx - 1) * sizeof(MiscAnnotation));
    mm->count--;
    mm->dirty = true;
    if (mm->selected_idx == idx)     mm->selected_idx = -1;
    else if (mm->selected_idx > idx) mm->selected_idx--;
}
