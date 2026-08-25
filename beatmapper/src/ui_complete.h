#pragma once

#include "editor.h"
#include "audio.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "miscmap.h"
#include "lyricmap.h"
#include "undo.h"
#include "complete_algo.h"

#include "ui_tool.h"

// Complete Track tool.  Settings: every knob of the three stages; body: the
// task toggles, status and the ranked candidate list; actions: Analyze,
// select/deselect/discard, second-stage smoothing, Accept.  Exposes the
// proposal so the timeline can draw it as ghosts.
void ui_complete_settings(ToolCtx& c);
void ui_complete_body(ToolCtx& c);
void ui_complete_actions(ToolCtx& c);

// Opening the tool with a region selected and no proposal yet runs the
// analysis right away, so select-then-click shows results.
void ui_complete_auto_analyze(ToolCtx& c);

// Hotkeys: A runs the analysis (open the tool first via ui_dock_icon_click);
// C accepts the ticked candidates.
void ui_complete_hotkey_analyze(ToolCtx& c);
void ui_complete_hotkey_accept(ToolCtx& c);

// Call when the panel is not rendered this frame (drops the hover highlight).
void ui_complete_hidden();

// Drop the proposal (new track loaded).
void ui_complete_reset();

// --- for the timeline ------------------------------------------------------

// Never null.  Candidates are drawn only while ui_complete_ghosts_active().
const CompleteProposal* ui_complete_proposal();
bool ui_complete_ghosts_active();

// Candidate index under the mouse in the list, or -1.
int  ui_complete_hover();

// False for candidates hidden by the region filter: not listed, not drawn.
bool ui_complete_cand_listed(int idx);

// Ghost colour for a candidate given its state: three levels of presence --
// faint when deselected, solid when selected, brightest (and a different hue)
// under the mouse.
unsigned int ui_complete_ghost_color(int idx);
