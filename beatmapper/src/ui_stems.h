#pragma once
#include "ui_tool.h"
#include "spectrogram.h"
#include "stems.h"

// Stems tool: runs the stem separator (stems.h) and lists the stems with
// the colours the timeline tabs use.  Behind a settings switch (off by
// default, still rough) it can show every stem's spectrogram as a
// translucent plane in 3D, the planes stacked in depth and the stack free to
// orbit; pixel alpha follows the spectrogram magnitude, so the planes behind
// show through.  The time span follows the timeline view; the frequency
// axis follows the timeline's Log / max-frequency controls.
void ui_stems_settings(ToolCtx& c);
void ui_stems_body(ToolCtx& c);
void ui_stems_actions(ToolCtx& c);

// The mix's own spectrogram, so it can be shown as a plane too.
void ui_stems_set_mix(SpectrogramState* mix);

// Settings row for a tool's analysis source: "<label>" then a toggle per
// stem (coloured) and a reset to the preset.  Draws nothing without stems.
// Returns true when the choice changed (the tool should re-run).
bool ui_stems_source_row(const char* label, StemSource* src, StemPreset preset);
// One dim line naming the source when it is not the mix, for a tool's body.
void ui_stems_source_note(StemSource* src, StemPreset preset, const char* verb);
