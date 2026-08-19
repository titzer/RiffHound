#pragma once

#include "editor.h"
#include "audio.h"

// Chroma Analyzer content (widgets only, no window).  Rendered by the tool
// dock into the drawer or a floating window.  Updates
// editor->chroma_hover_note each frame; the dock resets it when hidden.
void ui_chroma_content(EditorState* editor, AudioState* audio);
