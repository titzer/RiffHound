#pragma once

#include <vector>

// Unplaced lyrics are parked past the end of the audio (t_start >= the
// track's duration), one slot apart in lyric-sheet order.  Placing one
// leaves a hole in the slot numbering, which is how lyricmap_sheet_order
// knows where placed lyrics sit in the sheet.
#define LYRIC_SLOT 0.001

struct Lyric {
    double t_start;
    double t_end;
    char   text[128];
    bool   selected;   // member of the multi-selection that cut/copy/delete act on
};

struct LyricMap {
    Lyric* lyrics;
    int    count;
    int    capacity;
    bool   dirty;
    int    selected_idx;  // index of selected lyric, or -1
};

void lyricmap_init    (LyricMap* lm);
void lyricmap_shutdown(LyricMap* lm);
void lyricmap_clear   (LyricMap* lm);

// Add a lyric sorted by t_start. Swaps t_start/t_end if needed.
// Returns insertion index, or -1 on allocation failure.
int  lyricmap_add(LyricMap* lm, double t_start, double t_end, const char* text);

// Remove lyric at index idx.
void lyricmap_remove(LyricMap* lm, int idx);

// Split lyric at idx: text is split at cursor_pos, time range split at midpoint.
// *new_sel (if non-null) is set to the index of the second lyric after the split.
// audio_dur marks the placed/unplaced boundary: when the lyric being split is
// the LAST placed one, the first half keeps the whole interval and the second
// half becomes the next unplaced lyric instead of taking the interval's back
// half (pass 0 to disable that rule).
void lyricmap_split(LyricMap* lm, int idx, int cursor_pos, int* new_sel,
                    double audio_dur);

// Where the next pasted line parks: one slot past the last parked lyric.
double lyricmap_next_slot(const LyricMap* lm, double dur);

// Delete a parked lyric, moving the ones after it down a slot so the gap
// does not read as a placed lyric.  (Placed lyrics: same as lyricmap_remove.)
void lyricmap_remove_parked(LyricMap* lm, int idx, double dur);

// For each placed lyric, how many unplaced lyrics come before it in the
// lyric sheet, inferred from the holes in the parking slots; -1 when that
// can't be told.  seq has lm->count entries (unplaced ones get -1).
void lyricmap_sheet_order(const LyricMap* lm, double dur, int* seq);

// Put placed lyrics back among the unplaced ones, each into the parking slot
// it came from (by lyricmap_sheet_order's reckoning), or after the last
// when that can't be told.
void lyricmap_unplace(LyricMap* lm, const std::vector<int>& idxs, double dur);
