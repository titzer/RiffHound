# Chords, Patterns, and Riffs — proposed addendum to format-spec.md

**Status:** proposal, with working generator (`lib/rifftab/tab2ts.py`) and sample
output for three tracks.

This addendum answers two questions that block the Chord Buddy:

1. How are chords represented in a track file?
2. How is repetition represented, so that a player can show *"three chord loops,
   one of them played four times with a variation on the last pass"* instead of
   an unrolled wall of 400 chord events?

---

## 1. Design constraints

The existing format has properties worth not breaking:

- **Every line is `<start> <end> <name>`.** No nesting, no blocks. `grep` and
  `sort` work on it.
- **Unknown event names round-trip.** `beatmap_load` puts anything it does not
  recognise into `MiscMap` and writes it back verbatim, so new event kinds are
  safe to introduce before the editor understands them.
- **`BxN` already establishes the compression precedent.** A run of evenly
  spaced beats is written compressed and expanded on load. Chords and patterns
  should follow the same "compressed on disk, expanded in memory" pattern rather
  than inventing a different mechanism.
- **The beatmap is the clock.** Anything musical should be expressible in beats
  so that it survives retiming of the audio.

## 2. Chords

```
B33   B37   chord: Em
B37   B41   chord: D/F#
B41   B49   chord: C
```

- Start and end are **beat-relative** (`B<N>`). Harmony is musical, not
  physical; expressing it in beats means dragging a beat retimes the chord for
  free, which is exactly what `beatmap_retime_annotations` already does for
  other layers.
- The name is a standard chord symbol: root, quality suffix, optional `/bass`.
- Quality vocabulary: `` (major), `m`, `5`, `7`, `m7`, `maj7`, `sus2`, `sus4`,
  `dim`, `aug`, plus extensions as free text. A reader that does not understand
  a suffix should still parse the root.

> **Blocker:** `B<N>` times are in `format-spec.md` but **not implemented**.
> `beatmap.cpp:146` parses times with `sscanf("%lf %lf %63s")`, which fails on
> `B33`, and the line is silently dropped. Until that is fixed, generated files
> use seconds and carry the beat numbers in the trailing comment:
>
> ```
> 40.123456  42.456789  chord: Em   # ~beats=B33-B37 ~src=tab/gp5
> ```
>
> This is what the sample artifacts in this repo currently use, so that they
> load in today's editor without data loss.

### 2.1 `chart:` — the compressed form

Typing 400 chord events by hand is not reasonable, and neither is reading them.
`chart:` is a measure-grid shorthand that **expands deterministically into
`chord:` events**, exactly as `BxN` expands into `B` events:

```
B33   B49   chart: 4/4 | Am | G | F | E |
B49   B65   chart: 4/4 | Am G | F . . E |
```

- `|` separates measures. Within a measure, chord symbols are distributed evenly
  unless `.` is used to hold the previous chord for a beat.
- The number of measures must fit the beat span; a mismatch is an error the
  editor reports rather than guesses at.
- The editor expands on load and **recompresses on save**, so `chart:` and
  `chord:` are two encodings of one thing, not two sources of truth.

## 3. Patterns — the re-rolled layer

The observation driving this: songs are not sequences of chords, they are a
small number of **loops** played repeatedly with variations. A chord player
should show the loop, not the unrolling.

**A pattern is defined by its canonical occurrence in the track.**

```
# Definition: the pattern IS this range of the song.
B33   B65   pattern: verseLoop  @4/4
B33   B37   chord: Em
B37   B41   chord: D
B41   B45   chord: C
B45   B49   chord: D
...

# Instances elsewhere: NOT unrolled.
B65   B97   play: verseLoop
B97   B129  play: verseLoop  ~var=B13:Cmaj7
B225  B257  play: verseLoop  ~var=last-measure:D
```

Why define a pattern by pointing at a range of the track rather than declaring
it abstractly:

- It stays **synchronised to the audio**. The canonical instance is real music
  at a real time, so it can be played, looped, and inspected.
- It stays **editable in the existing editor** — it is just a time range, like a
  section, and gets snap-to-beat and drag behaviour for free.
- It **degrades gracefully**. A tool that ignores `pattern:`/`play:` still sees
  one complete instance of the chords plus some unrecognised ranges.
- It needs **no new namespace or symbol table** in the file.

An instance is expanded by beat arithmetic: for a `play:` starting at beat *S* of
a pattern whose definition starts at beat *P*, an event at beat *b* in the
definition appears at beat *b + (S − P)*. Instances need not be the same length
as the definition; a short instance is truncated and a long one repeats, which
is how a 4-bar loop covers an 8-bar verse.

### 3.1 Variations

`~var=` deltas are relative to the pattern's own beat numbering (beat 1 = the
first beat of the definition):

```
~var=B13:Cmaj7          replace the chord at pattern-beat 13
~var=B13-B16:drop       this range is not played
~var=last-measure:D     sugar for the final measure
```

This is what lets the player say *"the same loop, but the last time through it
resolves to D"* — which is the thing a musician actually needs to know, and the
thing an unrolled chart hides.

### 3.2 Riffs and melodies use the same mechanism

A pattern's content is whatever events fall inside its range. For a chord loop
that is `chord:` events; for a riff it is `note:` or RiffText `riff:` events:

```
B17   B25   pattern: mainRiff  @4/4
B17   B25   riff: gtr1 | 0e,3e,5q,7q,5e,3e,0h
B121  B129  play: mainRiff
B233  B241  play: mainRiff  ~var=B7-B8:drop
```

One mechanism covers chord loops, riffs, drum patterns, and melodic phrases.
This is also precisely the "repeat tool" already described in `editor-spec.md`
lines 59–60 ("the section is not replicated, but repeated, and the start of each
repeat can be manually adjusted"), so the editor and the format agree.

## 4. Note events (machine layer)

Dense transcription output — from a tab, a pitch tracker, or a transcription
model — is not human-scale and does not belong in the combined file:

```
41.203  41.512  note: gtr1 p=64 s=6 f=0 v=90
```

`p` = MIDI pitch, `s`/`f` = string/fret when known, `v` = velocity. These live
in a sidecar (`<track>.notes.txt`), because a full transcription is tens of
thousands of events and `track.txt` is a file a human edits.

## 5. Provenance

Machine-generated events carry their origin in the trailing comment:

```
B33  B37  chord: Em   # ~src=tab/gp5+chroma ~conf=0.86
```

Hand-authored events carry nothing, so absence of `~src` means a human did it.
This requires one editor change: preserve the trailing comment per event on load
(today `beatmap.cpp:135` strips it), so provenance survives a GUI round-trip.

## 6. Summary of what the editor must learn

| Change | Size | Needed for |
|---|---|---|
| Parse `B<N>` times | small | chords/patterns surviving retiming |
| Preserve per-event trailing comments | small | provenance, confidence |
| Expand/recompress `chart:` | medium | hand-authoring chord charts |
| Render `chord:` lane on the timeline | medium | seeing chords against audio |
| Render `pattern:`/`play:` as linked ranges | medium | the re-rolled view |
| Proposal-layer review (accept/reject) | medium | consuming AI output safely |

None of these are prerequisites for *producing* the data — the sample artifacts
already exist and load today as misc annotations.
