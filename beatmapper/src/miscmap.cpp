#include "miscmap.h"
#include "beatmap.h"
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
    ma.t_start  = t_start;
    ma.t_end    = t_end;
    ma.selected = false;
    ma.text[0]  = '\0';
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

void miscmap_clear_selection(MiscMap* mm) {
    for (int i = 0; i < mm->count; i++) mm->entries[i].selected = false;
}

int miscmap_selected_count(const MiscMap* mm) {
    int n = 0;
    for (int i = 0; i < mm->count; i++) if (mm->entries[i].selected) n++;
    return n;
}

int miscmap_move_selection(MiscMap* mm, double dt, int* focus_idx) {
    int n = miscmap_selected_count(mm);
    if (n == 0) return 0;

    // Lift the whole group out, then re-add it: entries are kept sorted by
    // t_start, and a move can reorder the group against what it moves past.
    MiscAnnotation* moved = (MiscAnnotation*)malloc(n * sizeof(MiscAnnotation));
    if (!moved) return 0;
    int  focus_in = focus_idx ? *focus_idx : -1;
    int  focus_at = -1;                       // position of the focus within moved[]
    int  k = 0;
    for (int i = 0; i < mm->count; i++) {
        if (!mm->entries[i].selected) continue;
        if (i == focus_in) focus_at = k;
        moved[k++] = mm->entries[i];
    }
    for (int i = mm->count - 1; i >= 0; i--)
        if (mm->entries[i].selected) miscmap_remove(mm, i);

    if (focus_idx) *focus_idx = -1;
    for (int i = 0; i < n; i++) {
        int idx = miscmap_add(mm, moved[i].t_start + dt, moved[i].t_end + dt, moved[i].text);
        if (idx < 0) continue;
        mm->entries[idx].selected = true;
        if (focus_idx && i == focus_at) *focus_idx = idx;
    }
    free(moved);
    mm->dirty = true;
    return n;
}

// --- clipboard -----------------------------------------------------------

#define MISC_CLIP_MAX 512

struct MiscClipItem {
    double dt_start, dt_end;   // seconds, relative to the group's first start
    double db_start, db_end;   // beats, relative to the group's first start
    char   text[128];
};

static MiscClipItem s_clip[MISC_CLIP_MAX];
static int          s_clip_count = 0;
static bool         s_clip_beats = false;  // beat offsets are meaningful

int miscmap_clipboard_count() { return s_clip_count; }

int miscmap_copy_selection(const MiscMap* mm, const BeatMap* bm) {
    if (miscmap_selected_count(mm) == 0) return 0;  // keep what is already held

    s_clip_count = 0;
    s_clip_beats = (bm && bm->count >= 2);
    double t0 = 0.0, b0 = 0.0;
    bool   first = true;
    for (int i = 0; i < mm->count && s_clip_count < MISC_CLIP_MAX; i++) {
        const MiscAnnotation& ma = mm->entries[i];
        if (!ma.selected) continue;
        if (first) {
            t0 = ma.t_start;
            b0 = s_clip_beats ? beatmap_beat_pos(bm, t0) : 0.0;
            first = false;
        }
        MiscClipItem& c = s_clip[s_clip_count++];
        c.dt_start = ma.t_start - t0;
        c.dt_end   = ma.t_end   - t0;
        c.db_start = s_clip_beats ? beatmap_beat_pos(bm, ma.t_start) - b0 : 0.0;
        c.db_end   = s_clip_beats ? beatmap_beat_pos(bm, ma.t_end)   - b0 : 0.0;
        memcpy(c.text, ma.text, sizeof(c.text));
    }
    return s_clip_count;
}

int miscmap_paste(MiscMap* mm, const BeatMap* bm, double t_anchor) {
    if (s_clip_count == 0) return 0;
    bool   use_beats = s_clip_beats && bm && bm->count >= 2;
    double b_anchor  = use_beats ? beatmap_beat_pos(bm, t_anchor) : 0.0;

    miscmap_clear_selection(mm);
    int pasted = 0;
    for (int i = 0; i < s_clip_count; i++) {
        const MiscClipItem& c = s_clip[i];
        double ts, te;
        if (use_beats) {
            ts = beatmap_time_at(bm, b_anchor + c.db_start);
            te = beatmap_time_at(bm, b_anchor + c.db_end);
        } else {
            ts = t_anchor + c.dt_start;
            te = t_anchor + c.dt_end;
        }
        int idx = miscmap_add(mm, ts, te, c.text);
        if (idx < 0) break;
        mm->entries[idx].selected = true;
        pasted++;
    }
    return pasted;
}
