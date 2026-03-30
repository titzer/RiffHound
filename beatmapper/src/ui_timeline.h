#pragma once

#include "editor.h"
#include "audio.h"
#include "spectrogram.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "undo.h"

// Main timeline widget: spectrogram + time ruler + beat markers + section overlays.
// Handles its own scroll/zoom input.

void ui_timeline_render(EditorState* editor, AudioState* audio,
                        SpectrogramState* spectro, BeatMap* beatmap,
                        UndoStack* undo, SectionMap* sectionmap);
