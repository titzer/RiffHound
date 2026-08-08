#pragma once

#include "miscmap.h"
#include "panels.h"

struct EditorState;
struct BeatMap;
struct UndoStack;
struct ImDrawList;

// --- annotation strips ----------------------------------------------------
// Two lanes of the same thing -- ranges of time carrying a line of text -- and
// so one implementation with two instances rather than two copies of six
// hundred lines of drag handling.  What differs is the keyword each lane owns
// on disk, its colours, and its label; everything a user does to one they can
// do to the other.
//
// Chords get their own lane because they are the annotation a chart is made of:
// typing "chord:" four hundred times is not authoring, and a chord sharing a
// row with a strum pattern hides both.  The lane adds the keyword on save, so
// what is typed is "Em" and what is written is "chord: Em".
//
// Both lanes lay out on as many rows as their height allows: annotations that
// overlap in time are stacked instead of drawn on top of each other, which is
// what makes a lane readable once more than one kind of thing lives in it.

enum AnnStripId { ANN_CHORDS = 0, ANN_MISC, ANN_COUNT };

// Everything a strip needs from the rest of the frame.  Passed rather than
// stored, because all of it changes every frame.
struct AnnStripCtx {
    EditorState*   editor;
    const BeatMap* beatmap;
    UndoStack*     undo;
    double         playhead;   // paste anchor
};

void     annstrip_init(MiscMap* chords, MiscMap* misc);
MiscMap* annstrip_map(AnnStripId id);
PanelId  annstrip_panel(AnnStripId id);

// The height this lane is currently set to, in pixels.  Editable by dragging
// the lane's bottom edge; the value lives in EditorState so it survives a
// track change and is one of the things the Settings popup can reach.
float annstrip_height(const EditorState* e, AnnStripId id);
int   annstrip_rows  (float height);        // rows that fit in that height

// Geometry and row layout for this frame.  Call before hit-testing: which
// annotation a click lands on depends on which row it was drawn in.
void annstrip_place(AnnStripId id, const EditorState* e, bool show,
                    float x, float y, float w, float h);

// Mouse-down inside the lane.  Returns true when the lane took the click, in
// which case the caller should not also treat it as a seek.
bool annstrip_click(AnnStripId id, const AnnStripCtx& c, float mx, float my);

// A click landed somewhere else: drop this lane's selection and inline edit.
void annstrip_defocus(AnnStripId id);

// Per-frame drag tracking and release handling.
void annstrip_drag(AnnStripId id, const AnnStripCtx& c);

// Delete / copy / cut / paste.  Acts on the lane last clicked in, so one set of
// keys serves both without either having to be "the" lane.
void annstrip_keys(const AnnStripCtx& c);

void annstrip_draw(AnnStripId id, const AnnStripCtx& c, ImDrawList* dl,
                   float label_x);

// The inline text editor, an InputText laid over the focused annotation.  Adds
// ImGui widgets, so it must run outside the draw-list-only passes.
void annstrip_edit(AnnStripId id, const AnnStripCtx& c);

// Mouse released anywhere: end whatever the lanes were tracking.
void annstrip_release_all();

// True while a lane's bottom edge is being dragged, so the timeline can leave
// the cursor and the playhead alone.
bool annstrip_resizing();
