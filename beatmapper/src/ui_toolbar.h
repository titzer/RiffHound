#pragma once

#include "editor.h"
#include "audio.h"

// Top toolbar: file open, playback controls, tool mode buttons.
// Phase 0: stubs for everything; file open sets audio->filename.

void ui_toolbar_render(EditorState* editor, AudioState* audio);
