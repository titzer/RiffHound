#pragma once
#include <stdint.h>
#include <vector>
#include "beat_chroma.h"   // AudioPcm

struct BeatMap;

// ---------------------------------------------------------------------------
// Onset timbre shapes.
//
// The first quarter-beat or so after a hit says what the hit *is* -- kick,
// snare, hat, strum, nothing -- far better than what pitch it carries.  A
// short spread-spectrum descriptor of that window (log-spaced bands over a
// few sub-frames, level-normalised) is clustered into a small track-specific
// vocabulary of "shapes".  Every onset in the track then gets a shape label,
// which turns the mapped beats into a rhythm pattern that can be matched
// elsewhere: a half-beat-off placement puts kick slots on hat onsets and
// scores badly, which tempo and chroma alone cannot tell.
// ---------------------------------------------------------------------------

static const int SHAPE_MAX_K = 8;

struct ShapeParams {
    int   k;                // vocabulary size (5)
    float window_frac;      // descriptor window as a fraction of the beat (0.25)
    int   slots_per_beat;   // rhythm grid resolution (12: straight and triplet feels)
    int   bands;            // log-spaced spectral bands (24)
    int   frames;           // temporal sub-frames inside the window (3)
    float low_weight;       // extra weight on bands below 200 Hz (kick) (1.5)
    float high_weight;      // extra weight on bands above 4 kHz (hat / snare) (1.5)
    float onset_threshold;  // detector threshold when harvesting onsets (1.0: quieter hits too)
};
void shape_params_defaults(ShapeParams* p);

// Descriptor of the audio in [t, t + win].  out has bands*frames entries,
// level-normalised; *energy is the window's RMS in dB (for loudness).
void shape_descriptor(const AudioPcm& a, double t, double win, const ShapeParams& p,
                      std::vector<float>* out, float* energy);

// Spectral shape of the audio in [t0, t1): mean log-band energy over
// log-spaced bands, with the mean across bands removed so that level drops
// out and what remains is the balance -- where the energy sits.  A verse
// with a singer and a solo over the same chords differ here, not in chroma.
void shape_band_profile(const AudioPcm& a, double t0, double t1, int bands,
                        std::vector<float>* out);

struct ShapeVocab {
    bool  valid = false;
    int   k = 0, dim = 0;
    std::vector<float> cent;     // k * dim, in standardised feature space
    std::vector<float> mean;     // per-dimension standardisation
    std::vector<float> inv_std;
    float scale = 1.0f;          // membership softmax sharpness
    std::vector<int>   count;    // training members per shape
    std::vector<float> energy;   // mean energy (dB) per shape
};

// K-means++ over n descriptors (feats is n*dim, row-major).  Shapes come out
// ordered loudest first so the labels read consistently.  False when n < k.
bool shape_train(const float* feats, const float* energies, int n, int dim, int k,
                 ShapeVocab* out);

// Nearest shape, and a soft membership (sums to 1) when soft is non-null.
int  shape_classify(const ShapeVocab& v, const float* feat, float* soft);

// --- Track-wide analysis ----------------------------------------------------
// Every onset in the track with its descriptor and shape, plus the vocabulary
// trained from the onsets inside mapped stretches.  Re-done only when the
// beatmap, the parameters or the detector change.

struct ShapeAnalysis {
    ShapeVocab          vocab;
    double              win = 0.0;          // descriptor window used (seconds)
    std::vector<double> onset_t;
    std::vector<float>  onset_energy;
    std::vector<int>    onset_shape;
    std::vector<float>  onset_soft;         // n * vocab.k
    std::vector<float>  onset_feat;         // n * dim (kept for re-training)
    // cache key
    bool   key_valid = false;
    int    key_beats = 0;
    double key_checksum = 0.0;
    ShapeParams key_params;
    int    key_algo = -1;
    uint64_t key_frames = 0;
};

// Make `sa` current for this audio + map.  beat_algo_idx selects the onset
// detector.  Returns true when the analysis is usable (has a vocabulary).
bool shape_analysis_ensure(ShapeAnalysis* sa, const AudioPcm& a, const BeatMap* bm,
                           double duration, const ShapeParams& p, int beat_algo_idx);
void shape_analysis_clear(ShapeAnalysis* sa);

// Index of the first onset at or after t.
int shape_onset_at(const ShapeAnalysis& sa, double t);

// The process-wide analysis the tools share (the Complete Track tool keeps it
// current; the Beat Detector classifies its beats against its vocabulary).
ShapeAnalysis* shape_track();
ShapeParams*   shape_params();

// --- Timbre strip marks ------------------------------------------------------
// What the timeline draws: one rectangle per classified window, exactly the
// width of the descriptor window, numbered with its shape.

enum ShapeSource { SHAPE_SRC_ONSET = 0, SHAPE_SRC_BEAT, SHAPE_SRC_PROPOSED, SHAPE_SRC_DETECTED,
                   SHAPE_SRC_COUNT };

struct ShapeMark {
    double t, win;
    int    shape;      // -1 = unclassified
    float  energy;
    float  conf;       // soft membership of the winning shape (0..1)
};

void shape_marks_set(ShapeSource src, const std::vector<ShapeMark>& marks);
void shape_marks_clear(ShapeSource src);
void shape_marks_clear_all();
const std::vector<ShapeMark>& shape_marks(ShapeSource src);

// Classify windows starting at each of the given times against the shared
// vocabulary and publish them under src (unclassified marks when there is no
// vocabulary yet, so the rectangles still show the window).
void shape_marks_classify(ShapeSource src, const AudioPcm& a, const double* times, int n,
                          double win);
