#include "undo.h"
#include <stdlib.h>
#include <string.h>

void undo_init(UndoStack* us) {
    memset(us, 0, sizeof(*us));
}

static void snapshot_free(UndoSnapshot* s) {
    free(s->beats);
    free(s->lyrics);
    free(s->sections);
    free(s->misc);
    memset(s, 0, sizeof(*s));
}

void undo_shutdown(UndoStack* us) {
    for (int i = 0; i < us->size; i++)
        snapshot_free(&us->slots[(us->head + i) % UNDO_MAX]);
    memset(us, 0, sizeof(*us));
}

void undo_clear(UndoStack* us) {
    undo_shutdown(us);   // frees every slot and zeroes the stack
}

// Copy count elements of elem_size bytes; leaves *out null on failure.
static void* dup_array(const void* src, int count, size_t elem_size) {
    if (!src || count <= 0) return nullptr;
    void* p = malloc((size_t)count * elem_size);
    if (p) memcpy(p, src, (size_t)count * elem_size);
    return p;
}

void undo_push(UndoStack* us, const BeatMap* bm, const LyricMap* lm,
               const SectionMap* sm, const MiscMap* mm) {
    if (us->size == UNDO_MAX) {
        // Discard oldest to make room
        snapshot_free(&us->slots[us->head]);
        us->head = (us->head + 1) % UNDO_MAX;
        us->size--;
    }
    int idx = (us->head + us->size) % UNDO_MAX;
    UndoSnapshot& s = us->slots[idx];
    memset(&s, 0, sizeof(s));

    s.has_beats    = (bm != nullptr);
    s.has_lyrics   = (lm != nullptr);
    s.has_sections = (sm != nullptr);
    s.has_misc     = (mm != nullptr);

    if (bm && bm->count > 0) {
        s.beats = (Beat*)dup_array(bm->beats, bm->count, sizeof(Beat));
        if (s.beats) s.beat_count = bm->count;
    }
    if (lm && lm->count > 0) {
        s.lyrics = (Lyric*)dup_array(lm->lyrics, lm->count, sizeof(Lyric));
        if (s.lyrics) s.lyric_count = lm->count;
    }
    if (sm && sm->count > 0) {
        s.sections = (Section*)dup_array(sm->sections, sm->count, sizeof(Section));
        if (s.sections) s.section_count = sm->count;
    }
    if (mm && mm->count > 0) {
        s.misc = (MiscAnnotation*)dup_array(mm->entries, mm->count, sizeof(MiscAnnotation));
        if (s.misc) s.misc_count = mm->count;
    }
    us->size++;
}

void undo_drop_last(UndoStack* us) {
    if (us->size == 0) return;
    us->size--;
    snapshot_free(&us->slots[(us->head + us->size) % UNDO_MAX]);
}

bool undo_pop(UndoStack* us, BeatMap* bm, LyricMap* lm,
              SectionMap* sm, MiscMap* mm) {
    if (us->size == 0) return false;
    us->size--;
    int idx = (us->head + us->size) % UNDO_MAX;
    UndoSnapshot& s = us->slots[idx];

    if (bm && s.has_beats) {
        free(bm->beats);
        bm->beats    = s.beats;
        bm->count    = s.beat_count;
        bm->capacity = s.beat_count;
        bm->dirty    = true;
        s.beats      = nullptr;
        s.beat_count = 0;
    }

    if (lm && s.has_lyrics) {
        free(lm->lyrics);
        lm->lyrics       = s.lyrics;
        lm->count        = s.lyric_count;
        lm->capacity     = s.lyric_count;
        lm->dirty        = true;
        lm->selected_idx = -1;
        s.lyrics         = nullptr;
        s.lyric_count    = 0;
    }

    if (sm && s.has_sections) {
        free(sm->sections);
        sm->sections     = s.sections;
        sm->count        = s.section_count;
        sm->capacity     = s.section_count;
        sm->dirty        = true;
        sm->selected_idx = -1;
        s.sections       = nullptr;
        s.section_count  = 0;
    }

    if (mm && s.has_misc) {
        free(mm->entries);
        mm->entries      = s.misc;
        mm->count        = s.misc_count;
        mm->capacity     = s.misc_count;
        mm->dirty        = true;
        mm->selected_idx = -1;
        s.misc           = nullptr;
        s.misc_count     = 0;
    }

    // Free anything the caller did not take ownership of.
    snapshot_free(&s);
    return true;
}

bool undo_can_undo(const UndoStack* us) {
    return us->size > 0;
}
