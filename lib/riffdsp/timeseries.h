#pragma once
// Timeseries file format (see beatmapper/format-spec.md).
//
// This is the extraction of the parse/serialize logic that currently lives
// inline in beatmapper/src/beatmap.cpp, so that non-GUI tools (and agents) can
// read and write the same files the editor does.

#include <stdio.h>

struct TsEvent {
    double t_start;
    double t_end;
    char   name[256];     // event name: everything after the end time
    char   comment[128];  // trailing "# ..." text, without the '#'; "" if none
    int    beat_start;    // >=0 when the source used B<N> form, else -1
    int    beat_end;
};

struct TsFile {
    TsEvent* events;
    int      count;
    int      capacity;
};

void ts_init    (TsFile* f);
void ts_shutdown(TsFile* f);

// Append an event.  Returns the new index, or -1 on allocation failure.
int  ts_add(TsFile* f, double t_start, double t_end, const char* name,
            const char* comment);

// Read a timeseries file.  Existing contents are cleared first.
// Expands "BxN" runs into individual "B" events, matching beatmap_load.
// Returns false if the file cannot be opened.
bool ts_load(TsFile* f, const char* path);

// Write events in file order.  Times are printed with 6 fractional digits,
// except events that carry beat numbers, which print as B<N>.
bool ts_save(const TsFile* f, const char* path);
void ts_write(const TsFile* f, FILE* out);

// Sort by start time, then end time.  Stable, so same-time events keep order.
void ts_sort(TsFile* f);

// --- convenience accessors -------------------------------------------------

// Collect beat times ("B" events) into a caller-allocated array, ascending.
// Returns the number written (never more than max).
int ts_beat_times(const TsFile* f, double* out, int max);

// True when the event name begins with prefix followed by end-of-name,
// a space, or a colon.  e.g. ts_is(e, "lyric") matches "lyric: foo".
bool ts_is(const TsEvent* e, const char* prefix);

// Text following "<prefix>:" in the event name, with whitespace trimmed.
// Returns "" when the event does not carry a payload.
const char* ts_payload(const TsEvent* e);
