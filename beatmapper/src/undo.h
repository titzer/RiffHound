#pragma once
#include "beatmap.h"

static const int UNDO_MAX = 64;

struct UndoSnapshot {
    Beat* beats;
    int   count;
};

// Circular-buffer undo stack.
// Each slot owns a heap-allocated copy of the beats array at a past state.
struct UndoStack {
    UndoSnapshot slots[UNDO_MAX];
    int          head;  // index of oldest entry
    int          size;  // number of valid entries (0..UNDO_MAX)
};

void undo_init(UndoStack* us);
void undo_shutdown(UndoStack* us);

// Snapshot the current beatmap before a mutating operation.
void undo_push(UndoStack* us, const BeatMap* bm);

// Restore the most recent snapshot into bm.  Returns false if stack is empty.
bool undo_pop(UndoStack* us, BeatMap* bm);

bool undo_can_undo(const UndoStack* us);
