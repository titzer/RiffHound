# Beatmap Editor — Implementation Plan

## Overview

A standalone native application for macOS and Linux that allows users to load audio
tracks and interactively produce beatmaps and section maps in the timeseries format
defined in `format-spec.md`. The emphasis is on fast, accurate beat placement with
bulk tools for interpolation and extrapolation, driven by a spectrogram view.

---

## Language, Platform, Build

- **Language**: C++ in a "C with structs" style — classes for data grouping, minimal
  inheritance, no templates beyond the standard library
- **Platforms**: macOS 12+ (CoreAudio via miniaudio), Linux (ALSA/PulseAudio via miniaudio)
- **Build**: `Makefile` with platform detection (`uname`), consistent with existing
  project style; `clang++` on macOS, `g++` on Linux

---

## Library Stack

| Library     | Purpose                                    | License      | Integration         |
|-------------|---------------------------------------------|--------------|---------------------|
| Dear ImGui  | Immediate-mode GUI, custom draw calls       | MIT          | vendored            |
| GLFW        | Cross-platform window + OpenGL context      | zlib         | vendored            |
| OpenGL 3.3  | Rendering backend for ImGui + textures      | system       | system              |
| miniaudio   | Audio playback, MP3/WAV/FLAC decode, seek   | MIT/Unlicense| single-header vendored |
| pffft       | FFT for spectrogram (already in project)   | BSD-like     | vendored            |

No other dependencies. All vendored libraries are either single-header or small
enough to include in-tree. Note: OpenGL is deprecated on macOS but functional
through at least macOS 15; Metal backend is a future option if needed.

---

## Directory Structure

```
beatmapper/
  src/
    main.cpp              -- entry point, window setup, main loop
    audio.cpp/h           -- miniaudio wrapper: load, play, pause, seek, PCM buffer
    spectrogram.cpp/h     -- STFT via pffft, tiled GPU texture cache, render
    timeseries.cpp/h      -- parse/serialize timeseries format (format-spec.md)
    beatmap.cpp/h         -- beat data model, load/save, all beat operations
    sectionmap.cpp/h      -- section data model, load/save
    editor.cpp/h          -- editor state: selection, scroll/zoom, undo/redo stack
    ui_timeline.cpp/h     -- main timeline widget (spectrogram + beat/section markers)
    ui_sections.cpp/h     -- section list panel
    ui_toolbar.cpp/h      -- toolbar, playback controls, tool mode selector
    ui_beattools.cpp/h    -- beat operation dialogs (interpolate, extrapolate, etc.)
  beatdetect/
    beatdetect.h          -- public C API (usable from C, C++, and future WASM)
    beatdetect.c          -- implementation: onset detection + beat tracking
  vendor/
    imgui/
    glfw/
    miniaudio/
    pffft/                -- already in project (webui/player/)
  Makefile
  format-spec.md
  editor-spec.md
  implementation-plan.md
```

---

## Module Descriptions

### `timeseries`
Parser and serializer for the line-oriented timeseries format. Handles:
- Comment stripping, blank line skipping
- Times in seconds (decimal) and beats (B-prefixed)
- Reading/writing `BxN` compressed beat runs
- Canonical sorting (by start time, then end time)
- Preserves comments attached to adjacent lines when sorting (as per format-spec)

### `beatmap`
In-memory beat representation. A beat is simply a `double` (seconds). Operations:
- Add, remove, move a single beat
- **Interpolate**: given two beat positions and a count N, insert N-2 evenly-spaced
  beats between them
- **Extrapolate forward/backward**: from the average interval of the last/first K beats,
  project N more beats forward or backward
- **Quantize**: re-interpolate all beats in a selection using the average BPM between
  the selection's first and last beat (snaps to a regular grid)
- Save: writes `BxN` runs where consecutive beats are evenly spaced within tolerance
- Load: expands `BxN` runs to individual beat positions

### `sectionmap`
In-memory section representation. A section has a type (one of the keywords in
format-spec), a start beat, end beat, and optional label suffix. Sections are always
stored relative to beats in the editor (even though the format supports seconds).

### `audio`
Thin wrapper around miniaudio:
- Load MP3/WAV from file, expose decoded PCM as `float[]` at a canonical sample rate
  (44100 Hz, mono mix for analysis; stereo for playback)
- Play/pause/seek by seconds
- Playback speed multiplier (0.5×–2×) via miniaudio's resampler
- Loop a time range

### `spectrogram`
- Compute short-time Fourier transform (STFT) using pffft over the loaded PCM
- Parameters: window size 2048, hop size 512 (≈ 11.6ms per column at 44100 Hz)
- Magnitude spectrum, converted to dB, mapped to a color (e.g. viridis or grayscale)
- Stored as tiled GPU textures (e.g. 512px wide tiles) for efficient rendering at any zoom
- Computed once after load (background thread to avoid blocking UI)
- Render: map current `view_start`/`view_end` to texture UV coordinates and blit

### `editor`
Owns the overall editor state:
- `view_start`, `view_end` (seconds) — the visible time window
- `selected_beats` — indices of currently selected beats
- `tool_mode` — enum: Select, Place, RegionSelect
- Undo/redo stack (command pattern, depth 100): each operation is a reversible command
  object that knows how to apply and undo itself

---

## Timeline View

The primary view: a scrollable, zoomable horizontal strip showing the spectrogram
with overlaid beat markers and section regions.

### Layout
```
+--[minimap strip: full track at low resolution, current view highlighted]--------+
+--[time ruler: tick marks at appropriate intervals for current zoom]-------------+
|                                                                                  |
|   [spectrogram texture, tiled]                                                  |
|                                                                                  |
|   |beat |beat    |beat         |beat                                            |
|   [========= section: verse 1 =========]  [=== chorus ===]                     |
|   ^playhead                                                                      |
+---------------------------------------------------------------------------------+
```

### Zoom and Scroll
- `view_start` and `view_end` in seconds define the visible window
- **Mouse wheel**: zoom in/out centered on cursor position
- **Click+drag on empty space**: pan
- **Minimum view width**: ~1 second (fits 2–3 beats at 200 BPM where beats are 300ms)
- **Maximum view width**: full track duration
- **Minimap strip** (top): low-resolution spectrogram of full track; a shaded rect
  shows the current view window; click or drag the rect to navigate — a lightweight
  substitute for the full stacked multi-zoom view

### Beat Markers
- Rendered as vertical lines using `ImDrawList::AddLine`
- Selected beats: highlighted (brighter color + slightly wider)
- Hovered beat: subtle highlight; shows time in tooltip
- Beat numbers shown below the ruler at coarser zoom levels

### Section Overlays
- Semi-transparent colored rects spanning section start/end beat positions
- Color is per section type (consistent with section list panel)
- Label ("verse 1", "chorus", etc.) shown centered in the rect when wide enough

### Interaction Modes (set from toolbar)

**Select mode** (default):
- Click a beat marker to select it (Shift+click to add to selection)
- Click+drag a beat marker to move it
- Click empty space to deselect

**Place mode**:
- Click empty space to add a beat at the cursor's time position
- Click an existing beat to delete it
- (Delete/Backspace in Select mode also deletes selected beats)

**Region Select mode**:
- Click+drag on empty space draws a selection rectangle; all beats within are selected
- After selection, bulk tools become available

---

## Beat Placement Tools

All bulk tools operate on the current beat selection (contiguous or not). Invoked
from the toolbar or a right-click context menu; a small inline dialog collects
parameters.

| Tool                  | Input                        | Action |
|-----------------------|------------------------------|--------|
| **Interpolate**       | 2 selected endpoints, count N | Insert N-2 evenly-spaced beats between them; replaces any beats already between |
| **Extrapolate fwd**   | K selected beats, count N     | Compute average interval from last K; append N beats forward |
| **Extrapolate bwd**   | K selected beats, count N     | Same, prepend N beats backward |
| **Quantize**          | Selected range                | Recompute all positions using average BPM (first-to-last), evenly spaced |
| **Nudge**             | Selected range                | Run beat finder on that audio region; shift each beat to nearest detected onset |

These are the primary productivity tools and should be implemented before the beat
detector (Nudge can be stubbed out initially and enabled once `beatdetect` is ready).

---

## Section Panel

A vertical list panel docked to the right:
- One row per section: `[color swatch] [type dropdown] [start beat] – [end beat] [delete]`
- Click a row to select it and scroll the timeline to that section
- "Add section" button appends a new section at the current playhead position
- Start/end beats are editable numeric fields
- Section type is a dropdown of the keywords from format-spec
- Saved to `<trackname>.sections.txt` on demand

---

## Playback Controls (toolbar strip below timeline)

- **Play/Pause** (Space)
- **Stop** (return to start)
- **Loop toggle** — loops the current visible window
- **Current position**: `mm:ss.sss` and `B<N>` (beat number from beatmap)
- **Local BPM**: computed from the two beats surrounding the playhead
- **Speed**: 0.5×, 0.75×, 1×, 1.25×, 1.5× via miniaudio resampler

---

## File Operations

- **Open track**: OS file dialog for `.mp3` or `.wav`; looks for sidecar files
  (`<name>.beatmap.txt`, `<name>.sections.txt`) in the same directory and loads them
  if present
- **Save beatmap**: writes `<name>.beatmap.txt` (sorted, with `BxN` compression)
- **Save section map**: writes `<name>.sections.txt`
- **Autosave**: writes `<name>.beatmap.autosave.txt` every 60 seconds and on any
  destructive operation (before the operation is applied, so it's a safety backup)
- All sidecar files are in the timeseries format from `format-spec.md` with times in
  seconds (not beats), for maximum portability

---

## Beat Detection Library (`beatdetect`)

A standalone C library with no external dependencies. Public API:

```c
// Analyze a mono float PCM buffer and return suggested beat times.
// Returns number of beats found; caller provides output array.
int beatdetect_find_beats(
    const float* pcm,       // mono float samples
    int          num_samples,
    int          sample_rate,
    float        hint_bpm,  // 0 = auto-detect
    double*      out_times, // caller-allocated array of beat times (seconds)
    int          max_beats
);
```

Algorithm:
1. **Onset detection**: compute onset strength signal using spectral flux — for each
   STFT hop, sum the positive-only differences in log-magnitude across frequency bins
2. **Tempo estimation**: autocorrelation of the onset strength signal to find the
   dominant periodicity (BPM); peak-picking in the expected 50–200 BPM range
3. **Beat tracking**: dynamic programming over the onset signal (Ellis 2007 approach)
   — find the sequence of beat positions that maximizes onset strength while penalizing
   deviation from the estimated inter-beat interval

The library is written in C so it can later be compiled to WASM (for web demos) and
called from Python (via ctypes) for algorithm development. It uses pffft internally
for its own STFT (does not depend on the app's spectrogram module).

---

## Library Index (deferred)

When implemented: a line-oriented tab-separated text file (`index.txt`) at the root
of the library directory, one record per track:

```
# dir_name <TAB> artist <TAB> title <TAB> duration_s <TAB> avg_bpm
hey_jude	The Beatles	Hey Jude	431.2	76.3
stairway	Led Zeppelin	Stairway to Heaven	480.8	82.1
```

Consistent with the project's line-oriented text-file philosophy. Fields can be
`grep`'d and `sort`'d without a database. Extended fields appended as needed.

---

## Implementation Phases

### Phase 1 — Foundation
- Makefile: platform detection, `clang++`/`g++`, ImGui + GLFW + OpenGL3 + miniaudio
- Window opens, ImGui demo visible
- `audio`: load MP3/WAV, play/pause/seek, expose PCM buffer
- `spectrogram`: STFT via pffft, render as GPU texture in a scrollable window
- `timeline`: time ruler, scrolling, zoom (mouse wheel), playhead synced to playback

### Phase 2 — Beat Editing
- `timeseries`: parser and serializer for the format
- `beatmap`: data model, add/move/delete beats
- Timeline: render beat markers, select/drag beats
- Save/load `.beatmap.txt`; undo/redo for beat operations

### Phase 3 — Bulk Tools and Navigation
- Region select (drag box)
- Interpolate, extrapolate forward/backward, quantize
- Minimap strip (full track thumbnail + view rect)
- Toolbar with tool mode buttons

### Phase 4 — Sections
- `sectionmap`: data model, add/edit/delete
- Section list panel
- Timeline section overlays (colored rects + labels)
- Save/load `.sections.txt`

### Phase 5 — Beat Detection
- Implement `beatdetect` library (onset detection + tempo estimation + beat tracking)
- Integrate: run on selected audio range, display suggested beats as a separate overlay
- Nudge tool: use beatdetect to adjust selected beats to nearest onset
- Full-track beat finder: run on entire track, present results for acceptance

### Deferred
- Playback speed control (miniaudio resampler integration is straightforward, low priority)
- Waveform view as alternative to spectrogram
- ID3 tag embedding for beatmap/section map
- Library indexing and fuzzy search
- Time signature display and measure grid
- WASM build of `beatdetect`
- Windows support (miniaudio and ImGui both support it; just needs Makefile target)
