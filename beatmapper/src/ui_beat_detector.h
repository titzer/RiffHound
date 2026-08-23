#pragma once

#include "editor.h"
#include "audio.h"
#include "beatmap.h"
#include "beat_algo.h"
#include "undo.h"

#include "ui_tool.h"

// Beat Detector, the detection half of the Beats tool.  Settings: algorithm
// and its knobs; body: status and detection state (re-detects when the region
// or settings change); actions: Detect / Select / Insert / Clear.
void ui_beat_detector_settings(ToolCtx& c);
void ui_beat_detector_body(ToolCtx& c);
void ui_beat_detector_actions(ToolCtx& c);

// Drop all detection results and forget the analysed window.  Called when a
// new track is loaded and whenever the region is cleared, so no stale beats
// linger on the timeline.
void ui_beat_detector_reset(AutoBeatList* autobeat);

// Per-frame housekeeping, called from the main loop regardless of whether the
// detector tool is on screen: clears results when there is no region, and
// when the tool is hidden, clears results that belong to a different region.
void ui_beat_detector_update(EditorState* editor, AutoBeatList* autobeat,
                             bool tool_visible);

// Ensure onset_times[] in autobeat covers [t1, t2].
// Uses current detector params. Runs detection if the range is not already covered.
// Called by the timeline when snap_interp_to_onsets is on and the fill
// range falls outside the last analyzed region.
void ui_beat_detector_ensure_onsets(AudioState* audio, BeatMap* beatmap,
                                    AutoBeatList* autobeat,
                                    double t1, double t2);
