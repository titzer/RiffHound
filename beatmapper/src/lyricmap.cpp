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

void lyricmap_split(LyricMap* lm, int idx, int cursor_pos, int* new_sel) {
    if (idx < 0 || idx >= lm->count) return;

    // Save lyric data before removal
    double t0 = lm->lyrics[idx].t_start;
    double t1 = lm->lyrics[idx].t_end;
    char   saved[128];
    strncpy(saved, lm->lyrics[idx].text, sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';

    int len = (int)strlen(saved);
    if (cursor_pos < 0)   cursor_pos = 0;
    if (cursor_pos > len) cursor_pos = len;

    // Build the two text halves
    char text_a[128] = {}, text_b[128] = {};
    int la = cursor_pos < 127 ? cursor_pos : 127;
    int lb = len - cursor_pos < 127 ? len - cursor_pos : 127;
    if (la > 0) strncpy(text_a, saved,              la);
    if (lb > 0) strncpy(text_b, saved + cursor_pos, lb);

    // Split the time range at the midpoint; for zero/tiny range (unplaced
    // lyrics), give each half a 1 ms slot so insertion order is preserved.
    bool   has_range = (t1 - t0 > 1e-4);
    double t_mid     = (t0 + t1) * 0.5;
    double ta0, ta1, tb0, tb1;
    if (has_range) {
        ta0 = t0;    ta1 = t_mid;
        tb0 = t_mid; tb1 = t1;
    } else {
        ta0 = t0;          ta1 = t0 + 0.001;
        tb0 = t0 + 0.001;  tb1 = t0 + 0.002;
    }

    lyricmap_remove(lm, idx);
    lyricmap_add(lm, ta0, ta1, text_a);
    int ib = lyricmap_add(lm, tb0, tb1, text_b);
    if (new_sel) *new_sel = ib;
}
