#pragma once

#include "editor.h"
#include "audio.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "miscmap.h"
#include "beat_algo.h"
#include "undo.h"

// Beat Smoothing floating panel.
// Operates on the index range spanned by the beat selection (select a whole
// range, or just a start and an end beat).  The panel continuously computes a
// preview of the proposed beat positions; the user accepts or ignores it.
// Accepting also drags along any section, lyric or misc annotation pinned to a
// beat that moves.
// Must be called outside the main docked window (same as ui_chroma_render).
void ui_smoothing_render(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                         SectionMap* sectionmap, LyricMap* lyricmap, MiscMap* miscmap,
                         UndoStack* undo, AutoBeatList* autobeat);

// Proposed positions awaiting acceptance, for the timeline to draw as ghosts.
// times[k] / orig[k] correspond to beat index i0 + k.
struct SmoothPreview {
    bool          active;
    int           i0, i1;      // inclusive beat index range
    int           n;           // i1 - i0 + 1
    const double* orig;        // beat times the preview was computed from
    const double* times;       // proposed beat times
};

// Never null; check ->active before use.
const SmoothPreview* ui_smoothing_preview();
