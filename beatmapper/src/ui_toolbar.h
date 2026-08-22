#pragma once

#include "editor.h"
#include "audio.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "miscmap.h"
#include "undo.h"
#include "recent.h"
#include "beat_algo.h"

// Top toolbar: file open, playback controls, tool mode buttons, interpolate panel.

void ui_toolbar_render(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                       UndoStack* undo, RecentFiles* recent, SectionMap* sectionmap,
                       LyricMap* lyricmap, MiscMap* miscmap, MiscMap* chordmap,
                       AutoBeatList* autobeat);

// Request the Open Audio dialog to appear on the next frame (callable from menu bar).
void ui_toolbar_open_dialog();
