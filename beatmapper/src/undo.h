#pragma once
#include "beatmap.h"
#include "lyricmap.h"

static const int UNDO_MAX = 64;

struct UndoSnapshot {
    Beat*  beats;
    int    beat_count;
    Lyric* lyrics;
    int    lyric_count;
};

// Circular-buffer undo stack.
// Each slot owns heap-allocated copies of beats and lyrics at a past state.
struct UndoStack {
    UndoSnapshot slots[UNDO_MAX];
    int          head;  // index of oldest entry
    int          size;  // number of valid entries (0..UNDO_MAX)
};

void undo_init(UndoStack* us);
void undo_shutdown(UndoStack* us);

// Snapshot the current beatmap and lyricmap before a mutating operation.
void undo_push(UndoStack* us, const BeatMap* bm, const LyricMap* lm);

// Restore the most recent snapshot into bm and lm.  Returns false if stack is empty.
bool undo_pop(UndoStack* us, BeatMap* bm, LyricMap* lm);

bool undo_can_undo(const UndoStack* us);
