#pragma once

#include "ui_tool.h"

// Right-edge tool dock: a slim icon rail that is always visible, plus a
// drawer holding the tools.  Each tool sits in its own framed box with a
// header (name, settings triangle, detach button); the box holds the tool's
// settings (when the triangle is open), its body, and its action buttons
// grouped at the bottom.  Clicking a rail icon opens the drawer with only
// that tool expanded; clicking it again closes the drawer.  A tool can be
// detached into a floating window and docked back.

enum DockTool {
    DOCK_CHROMA = 0,   // Chroma Analyzer: purely informative
    DOCK_BEATS,        // Beats: detector + smoother
    DOCK_LYRICS,       // Lyric Index
    DOCK_COMPLETE,     // Complete Track
    DOCK_RHYTHM,       // Rhythm Map: onset timbre shapes
    DOCK_TOOL_COUNT
};

// Total width reserved at the right edge of the main window this frame
// (icon rail + drawer when open).
float ui_dock_width();

// True when the tool's content is on screen: expanded in an open drawer, or
// detached as a floating window.
bool ui_dock_tool_visible(DockTool t);

// Rail-icon behaviour: open the drawer with only this tool expanded (the
// other docked tools collapse); if the tool is already expanded, close the
// drawer; if it is floating, focus its window.
void ui_dock_icon_click(DockTool t);

const char* ui_dock_tool_name(DockTool t);

// Render the rail, the drawer and any detached floating tools.  Call once per
// frame, outside the main docked window.
void ui_dock_render(EditorState* editor, AudioState* audio, BeatMap* beatmap,
                    UndoStack* undo, AutoBeatList* autobeat,
                    SectionMap* sectionmap, LyricMap* lyricmap,
                    MiscMap* miscmap, MiscMap* chordmap);
