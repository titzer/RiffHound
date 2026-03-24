#pragma once

// BeatMap: a sorted array of beat times (seconds).

struct BeatMap {
    double* times;
    int     count;
    int     capacity;
};

void beatmap_init(BeatMap* bm);
void beatmap_shutdown(BeatMap* bm);

// Sorted insert. Returns inserted index, or -1 if a beat already exists within 5 ms.
int  beatmap_add(BeatMap* bm, double t);

// Remove beat at index idx. No-op if idx is out of range.
void beatmap_remove(BeatMap* bm, int idx);

// Save to a timeseries file (format-spec.md).  Returns true on success.
bool beatmap_save(const BeatMap* bm, const char* path);

// Load from a timeseries file.  Clears existing beats first.
// Handles both "B" (single beat at t1) and "BxN" (N evenly-spaced beats t1..t2).
// Returns true on success.
bool beatmap_load(BeatMap* bm, const char* path);
