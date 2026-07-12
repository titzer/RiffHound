#pragma once

#include "editor.h"
#include "audio.h"
#include "beatmap.h"
#include "beat_algo.h"
#include "undo.h"

// Beat Detector floating panel.
// Renders the panel and triggers re-detection when the region or params change.
// Must be called outside the main docked window (same as ui_chroma_render).
void ui_beat_detector_render(EditorState* editor, AudioState* audio,
                             BeatMap* beatmap, UndoStack* undo,
                             AutoBeatList* autobeat);

// Ensure onset_times[] in autobeat covers [t1, t2].
// Uses current detector params. Runs detection if the range is not already covered.
// Called by the timeline when snap_interp_to_onsets is on and the fill
// range falls outside the last analyzed region.
void ui_beat_detector_ensure_onsets(AudioState* audio, BeatMap* beatmap,
                                    AutoBeatList* autobeat,
                                    double t1, double t2);
