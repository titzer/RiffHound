#pragma once

#include "editor.h"
#include "audio.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "miscmap.h"
#include "beat_algo.h"
#include "undo.h"

// Right-edge tool dock: a slim icon rail that is always visible, plus a
// drawer holding the four analysis tools stacked vertically.  Each tool can
// be collapsed with its header triangle, or detached into a floating window
// with the detach icon (and docked again with the floating window's Dock
// button).  Clicking a rail icon makes sure the drawer is open and that that
// tool is expanded; clicking it again closes the drawer.

enum DockTool {
    DOCK_CHROMA = 0,
    DOCK_DETECTOR,
    DOCK_SMOOTHING,
    DOCK_LYRICS,
    DOCK_TOOL_COUNT
};

// Total width reserved at the right edge of the main window this frame
// (icon rail + drawer when open).
float ui_dock_width();

// True when the tool's content is on screen: expanded in an open drawer, or
// detached as a floating window.
bool ui_dock_tool_visible(DockTool t);

// Rail-icon behaviour: open the drawer and expand the tool; if the tool is
// already expanded, close the drawer; if it is floating, focus its window.
void ui_dock_icon_click(DockTool t);

// Render the rail, the drawer and any detached floating tools.  Call once per
// frame, outside the main docked window.
void ui_dock_render(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                    UndoStack* undo, AutoBeatList* autobeat,
                    SectionMap* sectionmap, LyricMap* lyricmap,
                    MiscMap* miscmap, MiscMap* chordmap);
