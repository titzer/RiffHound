#pragma once

#include "editor.h"
#include "audio.h"
#include "spectrogram.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "undo.h"
#include "imgui.h"

// Main timeline widget: spectrogram + time ruler + beat markers + section/lyric overlays.
// Handles its own scroll/zoom input.

void ui_timeline_render(EditorState* editor, AudioState* audio,
                        SpectrogramState* spectro, BeatMap* beatmap,
                        UndoStack* undo, SectionMap* sectionmap,
                        LyricMap* lyricmap);

// Lyric font size control.  Call ui_timeline_set_lyric_fonts() once after the
// ImGui font atlas is populated (before the first frame) to register the fonts.
void ui_timeline_set_lyric_fonts(ImFont** fonts, int count, int default_idx);
void ui_timeline_lyric_font_larger();
void ui_timeline_lyric_font_smaller();
bool ui_timeline_lyric_font_can_grow();
bool ui_timeline_lyric_font_can_shrink();
