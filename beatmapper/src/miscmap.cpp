#include "miscmap.h"
#include "beatmap.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void miscmap_init(MiscMap* mm, const char* prefix) {
    mm->entries      = nullptr;
    mm->count        = 0;
    mm->capacity     = 0;
    mm->dirty        = false;
    mm->selected_idx = -1;
    mm->prefix       = prefix;
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
    // The annotations a run was tiling against are gone with the old track.
    miscmap_paste_chain_reset();
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
static const char*  s_clip_lane  = nullptr;  // prefix of the lane copied from

// Lanes are the same lane when they write the same keyword.  Compared by value
// rather than by pointer so the answer does not depend on where the literals
// happened to be interned.
static bool same_lane(const char* a, const char* b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

int miscmap_clipboard_count(const MiscMap* mm) {
    if (!mm || !same_lane(mm->prefix, s_clip_lane)) return 0;
    return s_clip_count;
}

int miscmap_copy_selection(const MiscMap* mm, const BeatMap* bm) {
    if (miscmap_selected_count(mm) == 0) return 0;  // keep what is already held

    miscmap_paste_chain_reset();  // a new clipboard starts a new run
    s_clip_count = 0;
    s_clip_lane  = mm->prefix;
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
    if (miscmap_clipboard_count(mm) == 0) return 0;
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

// --- paste chaining -------------------------------------------------------

static bool        s_chain_live = false;  // the next paste continues a run
static double      s_chain_from = 0.0;    // anchor the caller asked for last time
static double      s_chain_next = 0.0;    // where the continued paste lands
static const void* s_chain_map  = nullptr;  // lane the run is tiling into

// Two anchors this close are the same playhead: the transport advances by
// milliseconds per frame, so anything under this is float noise rather than
// the user deliberately moving somewhere else.
static const double CHAIN_EPS = 1e-4;

void miscmap_paste_chain_reset() { s_chain_live = false; }

// Where a paste anchored at `anchor` would end, from the clipboard alone.
// Mirrors miscmap_paste()'s choice of beats over seconds so the run tiles on
// the same grid the annotations themselves land on.
static double clip_group_end(const BeatMap* bm, double anchor) {
    bool   use_beats = s_clip_beats && bm && bm->count >= 2;
    double b_anchor  = use_beats ? beatmap_beat_pos(bm, anchor) : 0.0;

    double end = anchor;
    for (int i = 0; i < s_clip_count; i++) {
        double te = use_beats ? beatmap_time_at(bm, b_anchor + s_clip[i].db_end)
                              : anchor + s_clip[i].dt_end;
        if (te > end) end = te;
    }
    return end;
}

int miscmap_paste_chained(MiscMap* mm, const BeatMap* bm, double t_anchor) {
    if (miscmap_clipboard_count(mm) == 0) return 0;

    bool   chained = s_chain_live && s_chain_map == mm &&
                     fabs(t_anchor - s_chain_from) < CHAIN_EPS;
    double anchor  = chained ? s_chain_next : t_anchor;

    int pasted = miscmap_paste(mm, bm, anchor);

    // A short paste means miscmap_add() gave out, so there is no group edge to
    // tile against; so does a clipboard with no extent, where every paste of
    // the run would stack on the one before.  Both end the run.
    double end = clip_group_end(bm, anchor);
    if (pasted == s_clip_count && end > anchor) {
        s_chain_live = true;
        s_chain_from = t_anchor;
        s_chain_next = end;
        s_chain_map  = mm;
    } else {
        s_chain_live = false;
    }
    return pasted;
}
