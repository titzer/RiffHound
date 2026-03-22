#include "editor.h"
#include <math.h>

static const double MIN_VIEW_WIDTH = 1.0;  // seconds

void editor_init(EditorState* e) {
    e->view_start = 0.0;
    e->view_end   = 30.0;
    e->duration   = 180.0;
    e->tool_mode  = ToolMode::Select;
}

void editor_clamp_view(EditorState* e) {
    double max_end = (e->duration > 0.0) ? e->duration : 9999.0;
    double span    = e->view_end - e->view_start;

    // Clamp span without moving the window
    if (span < MIN_VIEW_WIDTH) span = MIN_VIEW_WIDTH;
    if (span > max_end)        span = max_end;

    // Shift window to stay within [0, max_end], preserving span
    if (e->view_start < 0.0)               e->view_start = 0.0;
    if (e->view_start + span > max_end)    e->view_start = max_end - span;
    e->view_end = e->view_start + span;
}

void editor_zoom(EditorState* e, float pixel_frac, float delta) {
    // delta > 0: zoom in (shrink view); delta < 0: zoom out
    double span   = e->view_end - e->view_start;
    double factor = pow(1.15, -delta);  // negative: scroll up shrinks view
    double pivot  = e->view_start + span * pixel_frac;
    e->view_start = pivot - (pivot - e->view_start) * factor;
    e->view_end   = pivot + (e->view_end   - pivot) * factor;
    editor_clamp_view(e);
}

void editor_pan(EditorState* e, double delta_sec) {
    e->view_start += delta_sec;
    e->view_end   += delta_sec;
    editor_clamp_view(e);
}
