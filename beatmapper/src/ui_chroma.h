#pragma once

#include "editor.h"
#include "audio.h"

// Render the floating Chroma Analyzer window.
// Updates editor->chroma_hover_note each frame (set to -1 when panel is hidden).
void ui_chroma_render(EditorState* editor, AudioState* audio);
