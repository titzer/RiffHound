#pragma once

#include "editor.h"
#include "audio.h"
#include "spectrogram.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "miscmap.h"
#include "beat_algo.h"
#include "undo.h"
#include "imgui.h"

// Main timeline widget: spectrogram + time ruler + beat markers + section/lyric overlays.
// Handles its own scroll/zoom input.

void ui_timeline_render(EditorState* editor, AudioState* audio,
                        SpectrogramState* spectro, BeatMap* beatmap,
                        UndoStack* undo, SectionMap* sectionmap,
                        LyricMap* lyricmap, AutoBeatList* autobeat);

// Drop per-track transient state (recorded taps, their smoothing preview,
// strip selections, a lyric hold in progress).  Call when a new track loads.
void ui_timeline_reset();

// --- taps, for the Beats tool ---------------------------------------------
// The tool's selection-based operations (smoothing, shift, subdivide, halve)
// treat selected taps like any other selected beat; this is the access the
// tool needs.  Times edited in place must stay chronological -- call
// ui_timeline_taps_sort() after edits that may reorder.
struct TapEntry { double time; bool selected; };
TapEntry* ui_timeline_taps(int* count);
void      ui_timeline_taps_sort();
bool      ui_timeline_tap_insert(double t);   // added selected; false when full
void      ui_timeline_tap_remove(int idx);

// Lyric Index content (widgets only, no window).  Rendered by the tool dock
// into the drawer or a floating window; shares selection state with the
// timeline's lyric strip.
void ui_timeline_lyric_index_content(EditorState* editor, AudioState* audio,
                                     BeatMap* beatmap, UndoStack* undo,
                                     LyricMap* lyricmap);

// True when holding L would record a lyric (playing, no region, and an
// unplaced lyric waiting) or is doing so now; main.cpp leaves the loop
// toggle alone in that case.
bool ui_timeline_lyric_hold_armed(const EditorState* editor, const AudioState* audio,
                                  const LyricMap* lyricmap);

// Lyric font size control.  Call ui_timeline_set_lyric_fonts() once after the
// ImGui font atlas is populated (before the first frame) to register the fonts.
void ui_timeline_set_lyric_fonts(ImFont** fonts, int count, int default_idx);
void ui_timeline_lyric_font_larger();
void ui_timeline_lyric_font_smaller();
bool ui_timeline_lyric_font_can_grow();
bool ui_timeline_lyric_font_can_shrink();
