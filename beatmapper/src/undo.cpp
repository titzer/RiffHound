#include "undo.h"
#include <stdlib.h>
#include <string.h>

void undo_init(UndoStack* us) {
    memset(us, 0, sizeof(*us));
}

void undo_shutdown(UndoStack* us) {
    for (int i = 0; i < us->size; i++) {
        int idx = (us->head + i) % UNDO_MAX;
        free(us->slots[idx].beats);
    }
    memset(us, 0, sizeof(*us));
}

void undo_push(UndoStack* us, const BeatMap* bm) {
    if (us->size == UNDO_MAX) {
        // Discard oldest to make room
        free(us->slots[us->head].beats);
        us->slots[us->head].beats = nullptr;
        us->slots[us->head].count = 0;
        us->head = (us->head + 1) % UNDO_MAX;
        us->size--;
    }
    int idx = (us->head + us->size) % UNDO_MAX;
    us->slots[idx].count = bm->count;
    us->slots[idx].beats = nullptr;
    if (bm->count > 0) {
        us->slots[idx].beats = (Beat*)malloc(bm->count * sizeof(Beat));
        if (us->slots[idx].beats)
            memcpy(us->slots[idx].beats, bm->beats, bm->count * sizeof(Beat));
        else
            us->slots[idx].count = 0;
    }
    us->size++;
}

bool undo_pop(UndoStack* us, BeatMap* bm) {
    if (us->size == 0) return false;
    us->size--;
    int idx = (us->head + us->size) % UNDO_MAX;

    free(bm->beats);
    bm->beats    = us->slots[idx].beats;
    bm->count    = us->slots[idx].count;
    bm->capacity = us->slots[idx].count;
    us->slots[idx].beats = nullptr;
    us->slots[idx].count = 0;
    bm->dirty = true;
    return true;
}

bool undo_can_undo(const UndoStack* us) {
    return us->size > 0;
}
