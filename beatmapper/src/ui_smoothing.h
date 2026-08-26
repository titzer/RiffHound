#pragma once

#include "editor.h"
#include "audio.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "miscmap.h"
#include "beat_algo.h"
#include "undo.h"

// Beat Smoothing content (widgets only, no window).  Rendered by the tool
// dock into the drawer or a floating window.
// Operates on the index range spanned by the beat selection (select a whole
// range, or just a start and an end beat).  The panel continuously computes a
// preview of the proposed beat positions; the user accepts or ignores it.
// Accepting also drags along any section, lyric, chord or misc annotation
// pinned to a beat that moves.
#include "ui_tool.h"
void ui_smoothing_settings(ToolCtx& c);   // presets and knobs
void ui_smoothing_body(ToolCtx& c);       // selection, preview statistics
void ui_smoothing_actions(ToolCtx& c);    // Accept / Clear selection

// Range edits on the selected beats: shift by +-1/2 beat (a stretch mapped
// half a beat off), subdivide x2 (accidental half-time), halve (accidental
// double-time).  Rendered as part of the Beats tool's actions.
void ui_beats_edit_actions(ToolCtx& c);

// Call when the panel is not rendered this frame, so the timeline preview
// ghosts are cleared.
void ui_smoothing_hidden();

// For the S hotkey: true when a computed preview would actually move a beat,
// and the accept action itself (same as the Accept smoothing button).
bool ui_smoothing_can_accept();
void ui_smoothing_accept(BeatMap* beatmap, UndoStack* undo, SectionMap* sectionmap,
                         LyricMap* lyricmap, MiscMap* miscmap, MiscMap* chordmap);

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

// The smoothing knobs in force (presets / sliders in the Beats tool), so
// other things -- the tap strip -- can smooth with the same settings.
const SmoothParams* ui_smoothing_params();
