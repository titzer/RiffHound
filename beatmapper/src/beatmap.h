#pragma once

struct Beat {
    double time;
    bool   selected;  // Interpolate tool multi-selection
    bool   interp;    // true = placed by Fill; false = fixed (hand-placed or committed)
};

struct BeatMap {
    Beat* beats;
    int   count;
    int   capacity;
    bool  dirty;          // true when beats differ from the last save/load
    char  save_path[512]; // path used for the last save or load; empty = never saved
};

void beatmap_init(BeatMap* bm);
void beatmap_shutdown(BeatMap* bm);

// Sorted insert.  New beat is fixed and not selected.
// Returns inserted index, or -1 if a beat already exists within 5 ms.
int  beatmap_add(BeatMap* bm, double t);

// Remove beat at index idx.  No-op if idx is out of range.
void beatmap_remove(BeatMap* bm, int idx);

// Save to a timeseries file (format-spec.md).
// On success all beats are committed (interp flags cleared).
bool beatmap_save(BeatMap* bm, const char* path);

// Load from a timeseries file.  Clears existing beats first.
// All loaded beats are fixed (interp=false).
bool beatmap_load(BeatMap* bm, const char* path);

// Mark all beats as fixed (clear all interp flags).
// Called automatically by beatmap_save on success.
void beatmap_commit(BeatMap* bm);

// Derive the companion beatmap path from an audio path by replacing the extension.
// e.g. /path/to/track.mp3  →  /path/to/track.txt
void beatmap_path_for_audio(const char* audio_path, char* out, int out_size);

// --- selection helpers ---------------------------------------------------

void   beatmap_clear_selection(BeatMap* bm);
int    beatmap_selected_count(const BeatMap* bm);

// Average BPM implied by the selected beats (treats them as consecutive beats).
// Returns 0 if fewer than 2 beats are selected.
double beatmap_selected_bpm(const BeatMap* bm);

// --- interpolation -------------------------------------------------------

// Insert evenly-spaced beats between t1 and t2 (exclusive) at the given BPM.
// The number of gaps is round((t2-t1)*bpm/60); does nothing when that is <= 1.
// Newly inserted beats are marked interp=true and not selected.
void beatmap_fill(BeatMap* bm, double t1, double t2, double bpm);
