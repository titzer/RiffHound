#pragma once

#include "editor.h"

// Uniform registry of the timeline strips (whose flags now mean expanded vs
// collapsed) and the dockable tool windows.  All state still lives in
// EditorState; this table is the single place that knows how each flag is
// named and presented, so adding a panel means adding one row to PANELS[] and
// nothing else.

enum PanelKind {
    PK_STRIP  = 0,  // horizontal lane inside the timeline
    PK_WINDOW = 1,  // floating tool window (Tools menu)
    PK_HELP   = 2,  // reference window     (Help menu)
};

enum PanelId {
    PANEL_INSERT = 0,
    PANEL_BEATS,
    PANEL_TEMPO,
    PANEL_TAPS,
    PANEL_AUTO,
    PANEL_SECTIONS,
    PANEL_LYRICS,
    PANEL_CHORDS,
    PANEL_MISC,
    PANEL_CHROMA,
    PANEL_DETECTOR,
    PANEL_SMOOTHING,
    PANEL_HELP,
    PANEL_COUNT
};

struct PanelDesc {
    PanelId     id;
    PanelKind   kind;
    const char* label;     // compact label
    const char* name;      // full name for menus
    const char* tip;       // hover text (nullptr = none)
    const char* shortcut;  // menu shortcut hint (nullptr = none)
    bool EditorState::* flag;
};

extern const PanelDesc PANELS[PANEL_COUNT];

bool panel_visible    (const EditorState* e, PanelId id);
void panel_set_visible(EditorState* e, PanelId id, bool v);
void panel_toggle     (EditorState* e, PanelId id);

// --- shared UI ------------------------------------------------------------

// Number of panels of a given kind.
int panels_count(PanelKind kind);

// Checkable menu items; call inside a BeginMenu()/EndMenu() block.
void panels_menu_items(EditorState* e, PanelKind kind);
