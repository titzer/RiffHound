#pragma once

#include "editor.h"
#include "audio.h"
#include "beatmap.h"
#include "lyricmap.h"
#include "undo.h"
#include <string>
#include <vector>

// "Find" in the Lyric Index: look the track's lyrics up online
// (lyrics_find.h) in the background.  With several results the button
// becomes "Results (N)" and a dialog lists them to pick from; the pick is
// added as unplaced lyrics, and a synced pick's timings become hints for
// auto-insert (ui_lyric_auto.h).

// Once per frame, whatever is on screen: land a finished search.  On a
// track's cache check, brings back the timings of the result picked before.
void ui_lyric_find_frame(const AudioState* audio);

// The button (and its dialog) in the Lyric Index header row.
void ui_lyric_find_button(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                          UndoStack* undo, LyricMap* lyricmap);

// New track: stop any search, forget the results, and check the cache for
// this one's.
void ui_lyric_find_track_loaded(const AudioState* audio);

// Search online (not just the cache) when the next track loads: the
// --find-lyrics launch flag.
void ui_lyric_find_request();

// Add lines as unplaced lyrics, parked after any already there.  Returns
// how many were added.  Shared with the Paste button.
int  lyric_add_unplaced(LyricMap* lyricmap, double dur, const std::vector<std::string>& lines);
