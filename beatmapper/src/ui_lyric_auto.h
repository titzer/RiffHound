#pragma once

#include "audio.h"
#include "lyricmap.h"
#include "lyrics_find.h"
#include <vector>

// Lyric auto-insert mode: with the track's stems separated, the unplaced
// lyrics are aligned to the vocal stem (lyric_align.h) and each gets a
// suggested position, drawn as a ghost on the lyric strip and offered in the
// Lyric Index to accept or dismiss.  Suggestions are recomputed whenever the
// lyrics change, so accepting one re-aligns the rest after it -- the first
// suggestion is by far the most reliable, so the intended flow is to accept
// from the top down (Tab).

struct LyricSuggestion {
    double key;        // t_start of the unplaced lyric it is for (its identity)
    char   text[128];  // ...and its text, to tell a stale suggestion
    double t0, t1;
    float  score;      // 0..1, how well the line's shape fits its phrase
};

bool ui_lyric_auto_active();
void ui_lyric_auto_set_active(bool on);

// Once per frame while the Lyric Index is shown: recompute when the lyrics,
// the stems or the track changed.  With no stems for the track yet, starts
// a separation (once per track).
void ui_lyric_auto_update(const AudioState* audio, const LyricMap* lyricmap);

// Why there are no suggestions (no stems, still separating...), or a summary.
const char* ui_lyric_auto_status();

// The suggestion for lyric `idx` (an unplaced one), or nullptr.
const LyricSuggestion* ui_lyric_auto_for(const LyricMap* lyricmap, int idx);

// All current suggestions, in lyric order (the first is the "next" one).
int                    ui_lyric_auto_count();
const LyricSuggestion* ui_lyric_auto_get(int i);

// Suggestion under the mouse in the Lyric Index (drawn brighter), or -1.
int  ui_lyric_auto_hover();
void ui_lyric_auto_set_hover(int i);

// The cursor: the suggestion L stepped to (drawn brightest, its lyric
// selected), which Tab accepts.  -1 when none.  After an accept the cursor
// moves on to the first suggestion after the accepted lyric.
int  ui_lyric_auto_cursor();
void ui_lyric_auto_advance();       // to the next suggestion in time (wraps)
void ui_lyric_auto_set_cursor(int i);
// A lyric was just placed ending at t: once the suggestions are recomputed,
// select the first one starting from there.
void ui_lyric_auto_continue_after(double t);
// True once after the cursor moved (L, or an accept carried it on): the
// Lyric Index then selects its span as the region and brings it into view.
bool ui_lyric_auto_take_cursor_moved();
// L steps through suggestions instead of placing / toggling the loop: the
// mode is on, there are suggestions, and playback is stopped.
bool ui_lyric_auto_wants_l(const AudioState* audio);

// Leave this lyric out of the alignment (its words aren't sung where the
// heuristics think, or at all).  Cleared when the mode is switched off.
void ui_lyric_auto_dismiss(double key);

// Move lyric `idx` to its suggestion.  The caller pushes undo.  Returns the
// lyric's new index, or -1 when it has no suggestion.
int  ui_lyric_auto_apply(LyricMap* lyricmap, int idx);

// Move every suggested lyric to its suggestion (caller pushes undo).
// Returns how many moved.
int  ui_lyric_auto_apply_all(LyricMap* lyricmap);

// Redo: the lyrics placed by accepting suggestions (this track, this
// session) that are still where they were put go back to being unplaced,
// in their places in the sheet, and get aligned again.  Lyrics placed or
// adjusted by hand stay as anchors.  The caller pushes undo.
int  ui_lyric_auto_redo_count(const LyricMap* lyricmap);
int  ui_lyric_auto_redo(LyricMap* lyricmap, double dur);

// Timed lyrics (from a lyrics lookup) for this track: they guide the
// alignment, and stand in for it until the vocal stem exists.
void ui_lyric_auto_set_hints(const std::vector<LyricTimedLine>& lines);
bool ui_lyric_auto_has_hints();

// New track: drop everything, hints included (the mode stays on).
void ui_lyric_auto_reset();
