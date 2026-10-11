#pragma once
#include "stems.h"

// Stem widgets shared by the analysis tools.  (Choosing what plays and
// shows is the stem tabs over the timeline spectrogram, in ui_timeline.)

// Settings row for a tool's analysis source: "<label>" then a toggle per
// stem (coloured) and a reset to the preset.  Draws nothing without stems.
// Returns true when the choice changed (the tool should re-run).
bool ui_stems_source_row(const char* label, StemSource* src, StemPreset preset);
// One dim line naming the source when it is not the mix, for a tool's body.
void ui_stems_source_note(StemSource* src, StemPreset preset, const char* verb);
