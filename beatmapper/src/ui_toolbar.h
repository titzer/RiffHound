#pragma once

#include "editor.h"
#include "audio.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "undo.h"
#include "recent.h"

// Top toolbar: file open, playback controls, tool mode buttons, interpolate panel.

void ui_toolbar_render(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                       UndoStack* undo, RecentFiles* recent, SectionMap* sectionmap);

// Request the Open Audio dialog to appear on the next frame (callable from menu bar).
void ui_toolbar_open_dialog();
