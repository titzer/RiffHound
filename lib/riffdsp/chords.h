#pragma once

// Chord recognition over beat-synchronous chroma.
//
// The unit of analysis is the beat, not a fixed window: a beatmap already says
// where the beats are, and harmony changes on beats.  Beat-synchronous chroma
// is both cheaper and markedly more accurate than frame-synchronous chroma
// followed by smoothing, because the averaging windows line up with the music.

static const int MAX_CHORD_BEATS = 8192;

enum ChordQuality : int {
    CQ_NONE = 0,   // N — no chord / silence
    CQ_5,          // power chord: root + fifth, no third
    CQ_MAJ,
    CQ_MIN,
    CQ_7,
    CQ_MIN7,
    CQ_MAJ7,
    CQ_SUS4,
    CQ_SUS2,
    CQ_DIM,
    CQ_COUNT
};

extern const char* const CHORD_QUALITY_SUFFIX[CQ_COUNT];
extern const char* const PITCH_CLASS_NAMES[12];

struct ChordLabel {
    int   root;      // 0..11 (C..B); -1 for CQ_NONE
    int   quality;   // ChordQuality
    float score;     // emission score of the winning template, 0..1
    float margin;    // winner minus runner-up; low margin = ambiguous
};

struct ChordParams {
    float  self_bonus;    // Viterbi reward for staying on the same chord
    float  root_weight;   // extra weight on the root pitch class
    float  bass_weight;   // extra weight on chroma from the low band
    float  silence_thresh;// below this chroma energy the beat is labelled N
    bool   allow_sevenths;
    bool   allow_sus;
    bool   allow_power;
};

void chord_params_defaults(ChordParams* p);

// Format a label as "Am", "G", "D7", "E5", or "N".
void chord_name(const ChordLabel* c, char* out, int out_size);

// Label a sequence of beat-synchronous chroma vectors.
//   chroma  : n * 12 floats, one 12-vector per beat interval
//   bass    : n * 12 floats from a low-pass band, or nullptr
//   out     : caller-allocated array of n labels
// Runs template matching per beat, then Viterbi over the whole sequence so
// that isolated one-beat flips are suppressed.
void chord_label_sequence(const float* chroma, const float* bass, int n,
                          const ChordParams* p, ChordLabel* out);
