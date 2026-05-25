# gpxdumper Plan

## Goal

A command-line tool (`gpxdumper`) that loads and parses Guitar Pro files (GP3/4/5) using
the existing `GpxParser` and `GpxIR`, then selectively displays song structure and content.

## Options

| Flag | Description |
|------|-------------|
| `--info` | Print song metadata (default when no other mode given) |
| `--list-tracks`, `-lt` | List tracks with string count and MIDI channel |
| `--markers` | Print rehearsal markers with measure numbers |
| `--beats` | Dump beats per measure, showing duration and note shorthand |
| `--notes` | Add per-note detail lines within `--beats` output |
| `-t N`, `--track N` | Filter to track N (1-indexed) |
| `-m M[-N]`, `--measures M[-N]` | Select measure range (e.g. `5` or `3-10`) |
| `-v 0\|1`, `--voice 0\|1` | Select voice (default: both) |

Multiple modes may be combined. Files are positional arguments.

## Output Samples

**`--info` (default):**
```
File:     song.gp5   Format: GP5   Title: ...   Tempo: 120 BPM   Tracks: 4   Measures: 32
```

**`--list-tracks`:**
```
Tracks:
   1: Lead Guitar             6 str  ch 1/1
   2: Bass                    4 str  ch 2/2
```

**`--markers`:**
```
Markers:
  M  1: Intro
  M  5: Verse
```

**`--beats` (per track → measure → voice → beat):**
```
=== Track 1: "Lead Guitar" (6 strings) ===
  M001 [4/4 q=75] <Intro>
    V0 B 1 [  960]: q    s2f5 s3f7
    V0 B 2 [ 1920]: e.   rest
    V0 B 3 [ 2640]: e    s1f0(t)
```

**`--notes` adds detail under each beat:**
```
      str=2 fret=5 vel=95 h-on slide
```

## Implementation

Single file `gpxdumper.main.v3` (~280 lines):
- Global option variables parsed in `component Main`
- `processFile(name)` → load bytes → `GpxParser.new(data).parse()` → dispatch to dump fns
- `dumpInfo`, `dumpTrackList`, `dumpMarkers`, `dumpBeats` → `dumpVoice` → `dumpNoteDetails`
- Helpers: `strEq`, `parseUint`, `parseMRange`, `printDur`, `printNote`, `pintW`, `pstrPad`

## Build

New `gpxdumper` target in `Makefile` using `v3c-host -program-name=gpxdumper`.
The `all` target builds both `gpxparser` and `gpxdumper`.
