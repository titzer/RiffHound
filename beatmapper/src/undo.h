#pragma once
#include "beatmap.h"
#include "lyricmap.h"
#include "sectionmap.h"
#include "miscmap.h"

static const int UNDO_MAX = 64;

// A snapshot covers only the layers the operation actually touches; layers it
// does not cover are left alone by undo_pop.  That keeps, say, a beat edit from
// resurrecting sections the user changed afterwards.
struct UndoSnapshot {
    Beat*           beats;
    int             beat_count;
    bool            has_beats;
    Lyric*          lyrics;
    int             lyric_count;
    bool            has_lyrics;
    Section*        sections;
    int             section_count;
    bool            has_sections;
    MiscAnnotation* misc;
    int             misc_count;
    bool            has_misc;
    MiscAnnotation* chords;
    int             chord_count;
    bool            has_chords;
};

// Circular-buffer undo stack.
// Each slot owns heap-allocated copies of the covered layers at a past state.
struct UndoStack {
    UndoSnapshot slots[UNDO_MAX];
    int          head;  // index of oldest entry
    int          size;  // number of valid entries (0..UNDO_MAX)
};

void undo_init(UndoStack* us);
void undo_shutdown(UndoStack* us);

// Drop every snapshot.  Call this whenever the maps are replaced wholesale
// (new audio file, beatmap loaded from disk) — snapshots taken against the old
// content would otherwise be restorable onto the new track.
void undo_clear(UndoStack* us);

// Snapshot the layers about to be mutated.  Pass nullptr for any layer the
// operation leaves alone.
void undo_push(UndoStack* us, const BeatMap* bm, const LyricMap* lm,
               const SectionMap* sm = nullptr, const MiscMap* mm = nullptr,
               const MiscMap* cm = nullptr);

// Discard the most recent snapshot without restoring it.
// Call this when a pushed operation turned out to be a no-op, so that Ctrl+Z
// never has to be pressed twice to see something happen.
void undo_drop_last(UndoStack* us);

// Restore the most recent snapshot.  Returns false if the stack is empty.
// Layers the snapshot does not cover, or that the caller passes as nullptr,
// are left untouched.
bool undo_pop(UndoStack* us, BeatMap* bm, LyricMap* lm,
              SectionMap* sm = nullptr, MiscMap* mm = nullptr,
              MiscMap* cm = nullptr);

bool undo_can_undo(const UndoStack* us);
