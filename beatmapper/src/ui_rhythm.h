#pragma once
#include "ui_tool.h"

// Rhythm Map tool: the onset timbre shapes (see onset_shape.h) as a thing of
// their own.  Settings own the shape parameters every other tool reads
// through shape_params(); the body shows the trained vocabulary with the
// colours the Timbre strip uses; Analyze trains it and publishes the marks.
void ui_rhythm_settings(ToolCtx& c);
void ui_rhythm_body(ToolCtx& c);
void ui_rhythm_actions(ToolCtx& c);

// Colour of a shape label in the timbre strip and the tool (shared palette).
unsigned int ui_rhythm_shape_color(int shape);
