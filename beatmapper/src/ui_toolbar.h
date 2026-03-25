#pragma once

#include "editor.h"
#include "audio.h"
#include "beatmap.h"
#include "undo.h"

// Top toolbar: file open, playback controls, tool mode buttons, interpolate panel.

void ui_toolbar_render(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                       UndoStack* undo);
