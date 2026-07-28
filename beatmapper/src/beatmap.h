#pragma once

struct SectionMap;  // forward declaration — see sectionmap.h
struct LyricMap;    // forward declaration — see lyricmap.h
struct MiscMap;     // forward declaration — see miscmap.h

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

// Save beats, sections, lyrics, and misc annotations to a combined timeseries file.
// On success all beats are committed and dirty flags are cleared.
// Pass sm/lm/mm=nullptr to omit that layer.
bool beatmap_save(BeatMap* bm, SectionMap* sm, LyricMap* lm, MiscMap* mm, const char* path);

// Load beats, sections, lyrics, and misc annotations from a combined timeseries file.
// Clears existing beats first; clears sm/lm/mm if non-nullptr.
// All loaded beats are fixed (interp=false).
bool beatmap_load(BeatMap* bm, SectionMap* sm, LyricMap* lm, MiscMap* mm, const char* path);

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

// Index range spanned by the selection: *i0 = lowest selected index,
// *i1 = highest.  Everything between them is part of the range even when not
// itself selected, so selecting just a start and an end beat works.
// Returns the number of selected beats (0 when nothing is selected).
int beatmap_selection_range(const BeatMap* bm, int* i0, int* i1);

// --- tempo statistics ----------------------------------------------------

struct BpmStats {
    int    n;        // number of inter-beat intervals measured
    double mean;     // mean instantaneous BPM
    double stddev;   // spread of the instantaneous BPM
    double min_bpm;
    double max_bpm;
};

// Statistics over the intervals of a chronological array of beat times.
BpmStats beatmap_bpm_stats(const double* times, int n);

// --- smoothing -----------------------------------------------------------

struct SmoothParams {
    float strength;      // 0..1: per-pass pull toward the local midpoint
    int   iterations;    // number of passes (more = closer to a constant tempo)
    bool  use_onsets;    // pull each moved beat toward a detected onset
    float onset_weight;  // 0..1: how hard onsets pull
    float onset_window;  // search radius as a fraction of the local beat period
    float max_shift;     // seconds; 0 = unlimited. Caps how far a beat may move.
};

void smooth_params_defaults(SmoothParams* p);

// Smooth beat timing in place over times[0..n-1], which must be chronological.
// The first and last entries are anchors and never move; interior beats are
// repeatedly pulled toward the midpoint of their neighbours, which evens out the
// instantaneous BPM.  When use_onsets is set, each pass also nudges beats toward
// the nearest entry in onsets[] (chronological) within onset_window of the local
// beat period, so audio evidence guides the result.
void beat_smooth_times(double* times, int n, const SmoothParams* p,
                       const double* onsets, int onset_count);

// Overwrite the times of beats [i0, i0+n) with times[].  The caller is
// responsible for keeping them chronological (beat_smooth_times does).
void beatmap_apply_times(BeatMap* bm, int i0, const double* times, int n);

// Carry annotations along when beats move: any section, lyric or misc endpoint
// that sits within tol of old_times[k] is retimed to new_times[k].  Endpoints
// that were not pinned to a beat are left where they are.  Pass nullptr for any
// layer to skip it.  Returns the number of endpoints adjusted.
int beatmap_retime_annotations(SectionMap* sm, LyricMap* lm, MiscMap* mm,
                               const double* old_times, const double* new_times,
                               int n, double tol);

// --- interpolation -------------------------------------------------------

// Insert evenly-spaced beats between t1 and t2 (exclusive) at the given BPM.
// The number of gaps is round((t2-t1)*bpm/60); does nothing when that is <= 1.
// Newly inserted beats are marked interp=true and not selected.
void beatmap_fill(BeatMap* bm, double t1, double t2, double bpm);
