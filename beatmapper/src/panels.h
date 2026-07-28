#pragma once

#include "editor.h"

// Uniform registry of everything the user can show or hide: timeline strips and
// floating tool windows.  All visibility state still lives in EditorState; this
// table is the single place that knows how each flag is named and presented, so
// adding a panel means adding one row to PANELS[] and nothing else.

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
    const char* label;     // compact label for the pane checkbox bar
    const char* name;      // full name for menus and the Settings popup
    const char* tip;       // hover text (nullptr = none)
    const char* shortcut;  // menu shortcut hint (nullptr = none)
    bool EditorState::* flag;
};

extern const PanelDesc PANELS[PANEL_COUNT];

bool panel_visible    (const EditorState* e, PanelId id);
void panel_set_visible(EditorState* e, PanelId id, bool v);
void panel_toggle     (EditorState* e, PanelId id);

// --- shared UI ------------------------------------------------------------
// One implementation each, used by the timeline pane bar, the Settings popup
// and the menu bar.

// Number of panels of a given kind.
int panels_count(PanelKind kind);

// Compact horizontal row of small checkboxes.
void panels_checkbox_row(EditorState* e, PanelKind kind);

// Vertical column of unlabelled checkboxes anchored at (x, y_top), one per
// row_h.  Each identifies itself through a tooltip, so the column fits in the
// timeline's narrow left lane.
void panels_checkbox_column(EditorState* e, PanelKind kind,
                            float x, float y_top, float row_h);

// One checkbox per line, full names (Settings popup).
void panels_checkbox_list(EditorState* e, PanelKind kind);

// Checkable menu items; call inside a BeginMenu()/EndMenu() block.
void panels_menu_items(EditorState* e, PanelKind kind);
