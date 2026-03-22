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
    double span = e->view_end - e->view_start;
    if (span < MIN_VIEW_WIDTH) {
        double mid = (e->view_start + e->view_end) * 0.5;
        e->view_start = mid - MIN_VIEW_WIDTH * 0.5;
        e->view_end   = mid + MIN_VIEW_WIDTH * 0.5;
    }
    double max_end = (e->duration > 0.0) ? e->duration : 9999.0;
    if (e->view_end > max_end) {
        e->view_start -= e->view_end - max_end;
        e->view_end    = max_end;
    }
    if (e->view_start < 0.0) {
        e->view_end  -= e->view_start;
        e->view_start = 0.0;
    }
    // Re-clamp end after start correction
    if (e->view_end > max_end) e->view_end = max_end;
}

void editor_zoom(EditorState* e, float pixel_frac, float delta) {
    // delta > 0: zoom in (shrink view); delta < 0: zoom out
    double span   = e->view_end - e->view_start;
    double factor = pow(1.15, delta);
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
