#pragma once

// Editor state: scroll position, zoom, duration, and region selection.
struct EditorState {
    double view_start;   // seconds – left edge of visible window
    double view_end;     // seconds – right edge of visible window
    double duration;     // total track duration (seconds)

    // Region selection – time range to feed to the beat analyzer.
    bool   has_region;
    double region_start;  // seconds
    double region_end;    // seconds

    bool   autoscroll;    // scroll timeline to follow playhead during playback

    // Playback parameters – reset to defaults on each new file load.
    float  speed;         // playback speed [0.25, 2.0], default 1.0
    int    semitones;     // pitch shift in semitones [-12, 12]
    int    cents;         // pitch fine-tune in cents  [-100, 100]
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
