#include "undo.h"
#include <stdlib.h>
#include <string.h>

void undo_init(UndoStack* us) {
    memset(us, 0, sizeof(*us));
}

static void snapshot_free(UndoSnapshot* s) {
    free(s->beats);
    free(s->lyrics);
    s->beats       = nullptr;
    s->beat_count  = 0;
    s->lyrics      = nullptr;
    s->lyric_count = 0;
}

void undo_shutdown(UndoStack* us) {
    for (int i = 0; i < us->size; i++)
        snapshot_free(&us->slots[(us->head + i) % UNDO_MAX]);
    memset(us, 0, sizeof(*us));
}

void undo_push(UndoStack* us, const BeatMap* bm, const LyricMap* lm) {
    if (us->size == UNDO_MAX) {
        // Discard oldest to make room
        snapshot_free(&us->slots[us->head]);
        us->head = (us->head + 1) % UNDO_MAX;
        us->size--;
    }
    int idx = (us->head + us->size) % UNDO_MAX;
    UndoSnapshot& s = us->slots[idx];
    s.beats       = nullptr;
    s.beat_count  = 0;
    s.lyrics      = nullptr;
    s.lyric_count = 0;

    if (bm && bm->count > 0) {
        s.beats = (Beat*)malloc(bm->count * sizeof(Beat));
        if (s.beats) {
            memcpy(s.beats, bm->beats, bm->count * sizeof(Beat));
            s.beat_count = bm->count;
        }
    }
    if (lm && lm->count > 0) {
        s.lyrics = (Lyric*)malloc(lm->count * sizeof(Lyric));
        if (s.lyrics) {
            memcpy(s.lyrics, lm->lyrics, lm->count * sizeof(Lyric));
            s.lyric_count = lm->count;
        }
    }
    us->size++;
}

bool undo_pop(UndoStack* us, BeatMap* bm, LyricMap* lm) {
    if (us->size == 0) return false;
    us->size--;
    int idx = (us->head + us->size) % UNDO_MAX;
    UndoSnapshot& s = us->slots[idx];

    // Restore beatmap
    free(bm->beats);
    bm->beats    = s.beats;
    bm->count    = s.beat_count;
    bm->capacity = s.beat_count;
    bm->dirty    = true;
    s.beats      = nullptr;
    s.beat_count = 0;

    // Restore lyricmap
    free(lm->lyrics);
    lm->lyrics      = s.lyrics;
    lm->count       = s.lyric_count;
    lm->capacity    = s.lyric_count;
    lm->dirty       = true;
    lm->selected_idx = -1;
    s.lyrics        = nullptr;
    s.lyric_count   = 0;

    return true;
}

bool undo_can_undo(const UndoStack* us) {
    return us->size > 0;
}
