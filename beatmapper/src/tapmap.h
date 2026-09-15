#pragma once

// The tap strip: beats tapped with T during playback, waiting to be smoothed,
// trimmed and inserted into the beat map.  A fixed-capacity map of its own so
// the undo stack can snapshot and restore it like any other layer.
static const int TAP_MAX = 1024;

struct TapEntry { double time; bool selected; };

struct TapMap {
    TapEntry taps[TAP_MAX];
    int      count;
};

// Append (unsorted; call tapmap_sort afterwards).  False when full.
bool tapmap_add(TapMap* tm, double t, bool selected);
void tapmap_remove(TapMap* tm, int idx);
void tapmap_sort(TapMap* tm);       // stable insertion sort by time
void tapmap_clear(TapMap* tm);
bool tapmap_any_selected(const TapMap* tm);
