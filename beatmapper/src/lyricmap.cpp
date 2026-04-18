#include "lyricmap.h"
#include <stdlib.h>
#include <string.h>

void lyricmap_init(LyricMap* lm) {
    lm->lyrics       = nullptr;
    lm->count        = 0;
    lm->capacity     = 0;
    lm->dirty        = false;
    lm->selected_idx = -1;
}

void lyricmap_shutdown(LyricMap* lm) {
    free(lm->lyrics);
    lm->lyrics   = nullptr;
    lm->count    = 0;
    lm->capacity = 0;
}

void lyricmap_clear(LyricMap* lm) {
    lm->count        = 0;
    lm->dirty        = false;
    lm->selected_idx = -1;
}

int lyricmap_add(LyricMap* lm, double t_start, double t_end, const char* text) {
    if (t_end < t_start) { double tmp = t_start; t_start = t_end; t_end = tmp; }

    if (lm->count >= lm->capacity) {
        int new_cap = (lm->capacity == 0) ? 16 : lm->capacity * 2;
        Lyric* tmp = (Lyric*)realloc(lm->lyrics, new_cap * sizeof(Lyric));
        if (!tmp) return -1;
        lm->lyrics   = tmp;
        lm->capacity = new_cap;
    }

    // Sorted insertion by t_start
    int pos = lm->count;
    for (int i = 0; i < lm->count; i++) {
        if (lm->lyrics[i].t_start > t_start) { pos = i; break; }
    }

    memmove(lm->lyrics + pos + 1, lm->lyrics + pos,
            (lm->count - pos) * sizeof(Lyric));

    Lyric& ly = lm->lyrics[pos];
    ly.t_start  = t_start;
    ly.t_end    = t_end;
    ly.text[0]  = '\0';
    if (text && text[0])
        strncpy(ly.text, text, sizeof(ly.text) - 1);
    lm->count++;
    lm->dirty = true;

    // Keep selected_idx valid after insertion
    if (lm->selected_idx >= pos) lm->selected_idx++;
    return pos;
}

void lyricmap_remove(LyricMap* lm, int idx) {
    if (idx < 0 || idx >= lm->count) return;
    memmove(lm->lyrics + idx, lm->lyrics + idx + 1,
            (lm->count - idx - 1) * sizeof(Lyric));
    lm->count--;
    lm->dirty = true;
    if (lm->selected_idx == idx)     lm->selected_idx = -1;
    else if (lm->selected_idx > idx) lm->selected_idx--;
}
