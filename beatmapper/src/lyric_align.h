#pragma once

#include <stdint.h>
#include <vector>

// Lyric alignment heuristics: place unplaced lyric lines on the vocal stem.
//
// The vocal stem is reduced to a loudness envelope, cut into "units" --
// stretches of singing between silences or deep loudness dips -- and the
// lines (in order) are matched to consecutive runs of units by dynamic
// programming.  A line's cost compares its syllable count with the run's
// length (at a singing rate fitted per track) and with the number of
// syllable-like loudness peaks in it, and charges for long pauses inside a
// line and for line breaks where the voice does not pause.  Units no line
// claims (ad-libs, backing vocals, stem bleed) can be skipped at a price.
//
// No speech recognition: this only knows how much is sung where.  Lines of
// similar length in a run of similar phrases can land one phrase off; the
// per-line score says how well the line's shape fits its phrase.

struct LyricActivity {
    double             hop = 0.01;     // seconds per envelope frame
    std::vector<float> db;             // loudness, dB (high-passed RMS, smoothed)
    float              floor_db = -90.0f;
    float              peak_db  = -90.0f;
};

// Envelope of a mono vocal signal.  Costs ~one pass over the samples.
void lyric_activity(const float* mono, uint64_t frames, uint32_t sample_rate,
                    LyricActivity* out);

// A stretch of singing.
struct LyricUnit {
    double t0, t1;
    double gap_before;   // silence before it (0 when split at a dip)
    float  dip_before;   // depth of the loudness dip before it, dB
    int    nuclei;       // syllable-like loudness peaks inside
};

// Defaults tuned with scripts/lyric-bench.sh --step (13 annotated tracks,
// 555 lines, 2026-10-11): the next line's start lands within 0.5 s 89 % of
// the time given the lines before it.  The loudness threshold matters most:
// stem bleed, reverb tails and backing vocals sit well under the lead, so
// singing is anything within 10 dB of the track's loud end.
struct LyricAlignParams {
    float  on_frac      = 0.33f;  // threshold between floor and peak, fraction
    float  max_below    = 10.0f;  // ...but never more than this many dB under the peak
    double bridge_gap   = 0.20;   // shorter silences are inside a word
    double min_unit     = 0.15;   // shorter blips are noise
    double split_len    = 1.2;    // units longer than this split at deep dips
    float  split_dip    = 7.0f;   // dB a dip must fall below both sides
    double rate_lo      = 0.14;   // seconds per syllable, search range
    double rate_hi      = 0.55;
    int    max_run      = 24;     // units one line may span
    float  w_dur        = 1.0f;   // (log length ratio)^2
    float  w_nuc        = 0.6f;   // (log syllable/peak ratio)^2
    float  w_inner      = 1.2f;   // per second of pause inside a line beyond inner_ok
    double inner_ok     = 0.50;
    float  w_break      = 1.5f;   // line break without a pause, per missing second
    double break_ok     = 0.30;
    float  w_skip       = 0.70f;  // per second of skipped singing
    float  c_skip       = 0.15f;  // per skipped unit
    float  c_unplaced   = 4.0f;   // per line left over at the end
    int    horizon      = 4;      // lines aligned at once (0: all); see lyric_align
    float  w_hint       = 2.0f;   // timed-lyrics hint: cost at hint_tol and beyond (saturates)
    double hint_tol     = 1.0;    // seconds past the slack where its cost saturates
    double hint_slack   = 0.0;    // seconds off the hint that cost nothing (LRC timing is rough)
};
void lyric_align_defaults(LyricAlignParams* p);

int lyric_syllables(const char* text);

// Units of singing in [t0, t1).
void lyric_units(const LyricActivity& act, double t0, double t1,
                 const LyricAlignParams& p, std::vector<LyricUnit>* out);

// A placed lyric.  Its span is taken out of the singing (no line goes
// there, none spans across it), and it calibrates the singing rate.  `seq`,
// when known, is how many of the lines being aligned come before it in the
// lyric sheet: those must fit before it and the rest after.  Unknown (-1),
// the alignment decides how many lines fit in the gap before it.
struct LyricAnchor {
    double      t0, t1;
    const char* text;
    int         seq = -1;
};

struct LyricAlignOut {
    bool   placed = false;    // false: ran out of singing before this line
    double t0 = 0, t1 = 0;
    float  score = 0;         // 0..1, how well the line fits its phrase
};

// Timed lyrics from a lyrics database (LRC): where each line starts in some
// recording of the song.  Only a hint: the recording may differ (intro
// length, an extra chorus), so a line pays for straying from its hint only
// up to a point.
struct LyricHintLine {
    double      t0;
    double      t1;      // < 0 when unknown
    const char* text;
};

// Match the lyric sheet to the hint lines: `lines` (in order) with the
// anchors whose sheet position is known (seq >= 0) between them, aligned
// in order to `hints` (an LCS on normalised text: case, punctuation and
// apostrophes ignored).  A line may also be the start or the end of a hint
// line (the user split it).  Placed lyrics only match hint lines within
// 20 s of themselves, which picks the right one among repeated choruses,
// and their median distance to their matches is the offset between the
// hints and this recording (0 with none).  *start gets each line's hinted
// start in this recording (offset applied), NAN where nothing matched.
// Returns the offset.
double lyric_match_hints(const std::vector<const char*>& lines,
                         const std::vector<LyricAnchor>& anchors,
                         const std::vector<LyricHintLine>& hints,
                         std::vector<double>* start);

// Align `lines` (in order) to the singing in [t0, t1) around the anchors.
// `hint` (optional, per line, NAN = none, already offset) pulls each line
// toward a start time.
// Returns the singing rate used (seconds per syllable).
//
// With a horizon, the lines are aligned a window at a time: the next
// `horizon` lines with an open end (singing after the window is free), of
// which the first half are kept before the window slides on.  A whole-song
// optimum happily misplaces the next line to fit lines a minute later
// better, which is the wrong trade for suggestions that are accepted from
// the top down.
double lyric_align(const LyricActivity& act, double t0, double t1,
                   const std::vector<const char*>& lines,
                   const std::vector<LyricAnchor>& anchors,
                   const LyricAlignParams& p, std::vector<LyricAlignOut>* out,
                   const std::vector<double>* hint = nullptr);
