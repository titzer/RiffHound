#pragma once

#include "editor.h"
#include "audio.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "miscmap.h"
#include "beat_algo.h"
#include "undo.h"
#include "tapmap.h"

// What every dockable tool is handed each frame.  Passed, not stored: all of
// it changes every frame.
struct ToolCtx {
    EditorState*  editor;
    AudioState*   audio;
    BeatMap*      beatmap;
    UndoStack*    undo;
    AutoBeatList* autobeat;
    SectionMap*   sectionmap;
    LyricMap*     lyricmap;
    MiscMap*      miscmap;
    MiscMap*      chordmap;
    TapMap*       taps;        // the tap strip (ui_timeline_tapmap())
    bool          in_drawer;   // docked in the side drawer (else a floating window)
};

// A tool is three optional parts the dock lays out inside one framed box:
//   settings  knobs, shown only while the header's triangle is open
//   body      what the tool shows; gets whatever height is left
//   actions   buttons, always grouped at the bottom of the box
typedef void (*ToolFn)(ToolCtx& c);
