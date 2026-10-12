#pragma once
// The chart: a beatmapper .txt sidecar (beatmapper/format-spec.md), read into
// what the karaoke screen draws -- lyric lines, chords, sections, loops -- and
// the bar grid the chords are laid on.
//
// Bars are the one thing here that is not simply read off the file.  The web
// client measures chords in seconds against one median bar length, which is
// why its grouping drifts; here everything is placed on the beat grid, and
// bars are counted in beats from anchors that are known to be downbeats.

#include <string>
#include <vector>

struct Lyric {
    double t0, t1;
    std::string text;
    double promote;     // when the line becomes the current one
    bool   cued;        // a gap long enough before it to earn a count-in
    double rest;        // when the current row goes blank, or -1 for never
};

struct Chord {
    double t0, t1;
    std::string name;
    int group;          // the chords: line it came from, or -1 for a single
};

struct Section {
    double t0, t1;
    std::string kind, label;
    int beats_per_bar;
};

struct Loop {
    double t0, t1;
    std::string name;
};

// One chord change inside a bar, at a beat offset from the bar's downbeat.
struct BarChord {
    double at;          // beats from the start of the bar
    std::string name;
    bool carried;       // the chord was already sounding when the bar began
};

struct Bar {
    double b0, b1;      // beat positions (fractional beat indices)
    double t0, t1;      // the same, in seconds
    int    section;     // index into sections, or -1
    int    number;      // 1-based bar number in the song
    std::vector<BarChord> chords;
};

// A run of up to four bars shown together; the chord row pages by block.
struct Block {
    int bar0, nbars;
};

struct ChartFlags {
    bool beats = false, chords = false, lyrics = false, loops = false,
         melody = false, trade = false;
};

struct Chart {
    std::vector<double>  beats;     // seconds
    std::vector<bool>    downbeat;  // BM, parallel to beats
    std::vector<Lyric>   lyrics;
    std::vector<Chord>   chords;
    std::vector<Section> sections;
    std::vector<Loop>    loops;
    double tuning = 440.0;          // the record's A4
    bool   tuning_from_map = false;
    double period = 0.5;            // median seconds per beat
    bool   synthetic_beats = false; // no beats in the file; a grid was made up
    std::vector<std::string> notes; // parse warnings, for the log

    std::vector<Bar>   bars;
    std::vector<Block> blocks;

    double beat_pos(double t) const;    // seconds -> fractional beat index
    double beat_time(double b) const;   // and back; extrapolates both ends
    double bpm() const { return period > 0 ? 60.0 / period : 0; }

    int lyric_at(double t) const;   // the current line: last whose promote <= t
    int bar_at(double t) const;
    int block_of(int bar) const;
    int section_at(double t) const;
};

// Parses a chart; duration (seconds, 0 if unknown) bounds the bar grid.
Chart chart_parse(const std::string& text, double duration);
// Rebuilds the bar grid for a known duration, e.g. once the audio is decoded.
void  chart_build_bars(Chart* c, double duration);
// What kinds of annotation a chart carries, without building anything.
ChartFlags chart_flags(const std::string& text);

bool read_file(const std::string& path, std::string* out);
