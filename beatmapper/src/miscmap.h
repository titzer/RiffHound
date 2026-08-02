#pragma once

struct MiscAnnotation {
    double t_start;
    double t_end;
    char   text[128];  // everything after t_start and t_end on the original line
    bool   selected;   // member of the multi-selection that cut/copy act on
};

struct MiscMap {
    MiscAnnotation* entries;
    int             count;
    int             capacity;
    bool            dirty;
    int             selected_idx;
};

void miscmap_init    (MiscMap* mm);
void miscmap_shutdown(MiscMap* mm);
void miscmap_clear   (MiscMap* mm);

// Add an entry sorted by t_start. Returns insertion index, or -1 on failure.
int  miscmap_add   (MiscMap* mm, double t_start, double t_end, const char* text);

// Remove entry at index idx.
void miscmap_remove(MiscMap* mm, int idx);

// Multi-selection helpers. `selected_idx` remains the focused entry -- the one
// inline editing and edge drags act on -- while the `selected` flags carry the
// group that cut/copy/delete act on.
void miscmap_clear_selection(MiscMap* mm);
int  miscmap_selected_count (const MiscMap* mm);

// Shift every selected annotation by dt seconds, keeping the array sorted and
// the selection intact.  focus_idx is an in/out index to follow across the
// re-sort (pass nullptr, or -1, when there is nothing to follow); it comes
// back as that annotation's new index.  Returns the number moved.
int miscmap_move_selection(MiscMap* mm, double dt, int* focus_idx);

// --- clipboard -----------------------------------------------------------
// One process-wide clipboard, so a group of annotations can be lifted from one
// verse and dropped on the next.  Offsets are stored relative to the group's
// first annotation in both seconds and beats; paste prefers beats, because a
// hand-tapped map of a live recording drifts and the same offsets in seconds
// would land the group progressively further off.

struct BeatMap;  // forward declaration -- see beatmap.h

// Copy every selected annotation.  Returns the number copied; a copy of 0
// leaves the previous clipboard contents alone.
int miscmap_copy_selection(const MiscMap* mm, const BeatMap* bm);

// Paste the clipboard with its first annotation starting at t_anchor.  The
// pasted group becomes the selection.  Returns the number pasted.
int miscmap_paste(MiscMap* mm, const BeatMap* bm, double t_anchor);

int miscmap_clipboard_count();
