#pragma once
#include "beatmap.h"
#include "sectionmap.h"
#include "miscmap.h"
#include "beat_chroma.h"
#include "onset_shape.h"
#include <vector>

// ---------------------------------------------------------------------------
// Complete Track: infer the unmapped remainder of a track from the parts that
// are already mapped.  Pure computation -- no ImGui -- so the stages can be
// driven by the tool panel today and swapped for external models later.
//
// Three stages, each producing ranked candidates the user previews and
// accepts one by one:
//
//   Beats     unmapped gaps are filled by *transferring* a mapped stretch whose
//             chroma matches the gap audio (a verse repeats its verse), gently
//             stretched to fit; where nothing matches, a constant-tempo grid
//             continues from the nearest mapped beats.  Either way the beats
//             are then pulled toward detected onsets and lightly smoothed.
//   Sections  mapped sections are matched against the rest of the beat grid
//             by beat-synchronous chroma similarity (measure granularity by
//             default) and proposed where they repeat.
//   Chords    a section without chords borrows the chart of the best-matching
//             section of the same kind, offsets travelling in beats.
//
// Sections and chords work from the beats *in the map*, so the workflow is:
// accept beats, re-run, accept sections, re-run, accept chords.
// ---------------------------------------------------------------------------

enum CandKind { CAND_BEATS = 0, CAND_SECTION, CAND_CHORDS, CAND_KIND_COUNT };

struct CompleteCand {
    CandKind kind;
    double   t0, t1;         // time span the candidate covers
    float    score;          // 0..1, what the list is ranked by
    float    sim;            // chroma similarity that produced it (0 = none)
    float    warp;           // beats: |stretch - 1| plus local warp after snapping
    bool     selected;       // accept checkbox state
    char     desc[128];      // one-line description for the list
    char     source[48];     // template it came from ("Verse 1", "beats 12-27")

    // CAND_BEATS: run of proposed beat times in CompleteProposal::beat_times
    int first, n;
    // CAND_SECTION (+ the template's chords when they spot-check: chord_n > 0)
    SectionKind sec_kind; char label[48]; int ts_num, ts_den;
    int   chord_first, chord_n;  // into CompleteProposal::chords
    float chord_sim;             // mean per-chord similarity of the spot check
    // CAND_CHORDS: run of chords in CompleteProposal::chords
    // (reuses first/n)
};

struct ChordProposal { double t0, t1; char text[128]; };

struct CompleteProposal {
    std::vector<double>       beat_times;  // all proposed beats, chronological
    std::vector<float>        beat_conf;   // 0..1 per proposed beat
    std::vector<ChordProposal> chords;
    std::vector<double>       onsets;      // detected onsets over every filled gap, sorted
    std::vector<CompleteCand> cands;       // ranked: beats, then sections, then chords
    int   gap_count;                       // stats for the status line
    int   template_count;
    char  status[160];
};

struct CompleteParams {
    bool do_beats, do_sections, do_chords;

    // --- beats ---
    float gap_factor;        // an interval > gap_factor * median IBI is a gap (1.8)
    int   template_beats;    // chunk length for templates cut from unsectioned beats (16)
    int   min_template_beats;// shortest stretch worth transferring (4)
    float max_warp;          // max |stretch - 1| tried when fitting a template (0.08)
    int   warp_steps;        // stretch factors tried across [-max_warp, +max_warp] (7)
    float warp_weight;       // score penalty per unit of warp (2.0)
    float jitter_beats;      // sub-beat start slack (0: stay on the grid)
    int   lookahead_beats;   // a template may start up to this many whole beats ahead (4)
    float lookahead_penalty; // score cost per beat of lookahead (0.1)
    float beat_sim_threshold;// transfer only when similarity - penalty >= this (0.55)
    int   beat_algo_idx;     // BEAT_ALGOS entry used for onsets
    int   fill_algo_idx;     // gap-fill strategy (see complete_fill_algo_name)
    float rhythm_weight;     // rhythm-shape: bonus added to the chroma score per unit rhythm match (1.0)
    float miss_penalty;      // rhythm-shape: cost of an expected hit with no onset (0.15)
    float extra_penalty;     // rhythm-shape: cost of a loud onset where none is expected (0.15)
    ShapeParams shape;       // onset timbre shapes (vocabulary size, window, ...)
    float det_min_bpm, det_max_bpm, det_threshold, det_tightness;
    float onset_weight;      // 0 = keep grid, 1 = land on the onset (0.5)
    bool  refit;             // refit each segment's stretch/offset to its onsets by least squares
    bool  grid_follow;       // tempo fills step along the seeded detector's beat grid
    float onset_window;      // search radius as a fraction of the beat (0.2)
    SmoothParams smooth;     // final light pass over each filled gap
    SmoothParams smooth2;    // second stage: per-segment smoothing on demand

    // --- sections ---
    int   section_granularity;   // 0 = measure, 1 = beat
    float section_sim_threshold; // (0.75)
    float section_max_overlap;   // fraction of a candidate allowed to overlap an existing section (0.1)
    float section_rhythm_weight; // blend of rhythm-map vs chroma in range comparisons (0.4)
    bool  section_discover;      // also find repeats by self-similarity (no template needed)
    bool  section_partition;     // infer uncovered spans as a partition (DP) rather than sliding matches
    float section_prior_weight;  // weight of the learned kind-transition prior in the DP (0.5; 0: off)
    bool  section_phase_lock;    // partition blocks start only on the measure phase the mapped sections share
    float section_timbre_weight; // weight of the per-beat spectral balance in section similarity (0: off)
    int   section_timbre_bands;  // log-spaced bands in that profile (16)
    float section_block_penalty; // fixed DP cost per block, against confetti partitions (1.0)
    float section_ext_penalty;   // DP cost per measure a block extends past its template (0.01)
    float section_trunc_penalty; // DP cost per measure a block truncates its template (0.04)
    int   section_min_measures;  // shortest repeat unit discovery will propose (4)
    float section_start_margin;  // discovery: shift a unit's start to a later measure only when it repeats this much better (<0: never shift)

    // --- chords ---
    bool  chord_runs;            // slide runs of mapped chords across the chord-free grid
    int   chord_run_beats;       // longest run used as a progression template (64)
    float chord_sim_threshold;   // min per-chord chroma+rhythm similarity for a run match (0.7)
    bool  chord_fallback;        // per-beat inference from learned chord models (or triads)
    float chord_margin;          // (unused by the decoder; kept for the UI)
    float chord_transition;      // decoder: cost of changing chord between beats (0.15)
    float chord_measure_bonus;   // decoder: fraction of that cost waived on measure starts (0.75)
    float chord_unseen_penalty;  // decoder: emission penalty for triads the map never used (0.1); >=1 disables them
    float chord_prior_beats;     // learned model = (n*learned + prior*triad)/(n+prior) (4)
    bool  chord_learn_rate;      // scale chord_transition by the map's median chord length / 4
    bool  chord_external;        // call the external learned chord model (madmom CNN+CRF)
                                 // via BM_CHORD_CMD or scripts/chords_madmom.py; needs audio_path
    float chord_external_blend;  // > 0: external labels become per-beat emission bonuses in the
    float chord_external_blend_cold; // the same bonus when the map has no chords to learn from (0: use chord_external_blend)
    float chord_corpus_weight;   // weight of the corpus-trained chord emission model in the decoder (0: off)
    float chord_corpus_weight_cold; // the same when the map has no chords to learn from
    float chord_key_bonus;       // emission bonus for triads diatonic to the song's key (0: off)
    int   chord_external_tool;   // 0: madmom, 1: ensemble of every installed model, 2: BTC (or $BM_CHORD_CMD)
                                 // decoder instead of standalone chord spans (0.25 is sensible)

    // --- chroma per beat ---
    BeatChromaParams chroma;
};
void complete_params_defaults(CompleteParams* p);

// Gap-fill strategies, selectable by index (fill_algo_idx).
int         complete_fill_algo_count();
const char* complete_fill_algo_name(int idx);
const char* complete_fill_algo_tip(int idx);

struct CompleteInputs {
    const BeatMap*    beatmap;
    const SectionMap* sectionmap;
    const MiscMap*    chordmap;
    AudioPcm          audio;
    const char*       audio_path;   // for external model calls (may be null)
    double            duration;
    bool              has_region;
    double            region_start, region_end;
};

// Run the enabled stages.  `cache` holds beat-synchronous chroma for the beats
// in the map and is kept current (invalidated per beat as beats move).
void complete_run(const CompleteInputs& in, const CompleteParams& p,
                  BeatChromaCache* cache, CompleteProposal* out);

// Second-stage smoothing: run beat_smooth_times over each proposed beat
// segment independently, pinned to the beats on either side of it (a mapped
// beat, or the neighbouring segment's edge beat) and guided by the onsets
// found during the fill.  Only selected segments when selected_only.
// Returns the number of segments smoothed.
int complete_smooth_segments(CompleteProposal* prop, const BeatMap* bm,
                             const SmoothParams& sp, bool selected_only);

// True when a candidate substantially intersects [r0, r1]: at least half of
// it lies inside, or the range lies inside it.
bool complete_cand_in_range(const CompleteCand& c, double r0, double r1);
