#pragma once

#include "editor.h"
#include "audio.h"

// Chroma Analyzer content (widgets only, no window).  Rendered by the tool
// dock into the drawer or a floating window.  Updates
// editor->chroma_hover_note each frame; the dock resets it when hidden.
#include "ui_tool.h"

// Chroma Analyzer: purely informative.  Settings: algorithm and rolling
// window; body: the twelve pitch-class bars for the region or the playhead.
void ui_chroma_settings(ToolCtx& c);
void ui_chroma_body(ToolCtx& c);
