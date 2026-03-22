#pragma once

// Editor state: the single source of truth for scroll position, zoom,
// tool mode, selection, and the undo/redo stack.
// Phase 0: view state and tool mode only.

enum class ToolMode {
    Select,
    Place,
    RegionSelect,
};

struct EditorState {
    double view_start;   // seconds – left edge of visible window
    double view_end;     // seconds – right edge of visible window
    double duration;     // total track duration (seconds)
    ToolMode tool_mode;
};

void editor_init(EditorState* e);

// Clamp view to valid range; enforce minimum view width.
void editor_clamp_view(EditorState* e);

// Zoom centered on a pixel position within the timeline.
// pixel_frac: fraction [0,1] of the timeline width where the cursor is.
// delta: positive = zoom in (narrower view), negative = zoom out.
void editor_zoom(EditorState* e, float pixel_frac, float delta);

// Pan by a time delta in seconds (can be negative).
void editor_pan(EditorState* e, double delta_sec);
