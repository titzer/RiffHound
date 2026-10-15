# RiffHound protocol — draft

Status: strawman, meant to be argued with. Nothing here is frozen yet.

There are already two clients — the web karaoke page in `webui/karaoke` and the
VT100 one in `textui/chords` — written months apart against no shared spec.
That accident is useful evidence, because they agree about what they need and
disagree about almost everything else. This document tries to write down the
agreement before it is lost.

Three things with very different stability requirements:

- the **notation**: what the music *is*. `rifftext`. Files on disk,
  hand-edited, outlives every client. Change rarely and carefully.
- the **score package**: notation plus what it cannot express — lyrics,
  fingerings, audio alignment.
- the **session**: what a group is doing *right now*. Ephemeral, both ends
  usually updated together, can keep churning.

Conflating them drags the session's churn into the file format.

## Principles

**1. One notation, many renderings.** Sheet music, tab, piano roll, harmonica
holes and clarinet fingering are not five kinds of data. They are five ways of
drawing the same notes. A client consumes the notation and renders what its
player asked for. Anything that would need a per-instrument copy of the music
is a design error.

**2. Carry the score, never the presentation.** The web client measures
syllable positions with `getBoundingClientRect`; the terminal counts
characters. Same data, incompatible layout, and neither ever wanted the
server's opinion. Cue countdowns, landing pads, strum glyphs, the choice
between a bouncing ball and a swinging hand — all derived client-side. The web
UI changed a dozen times while the data model barely moved; protect that ratio.

**3. Time is beats.** Beats survive the trainer features — slow down, loop,
half-time — without rewriting a single timing. Seconds don't. Beats are
canonical, a tempo map converts for audio, and a position is a beat.

**4. Structure is information, not compression.** A four-bar riff played three
times is a smaller thing to learn than twelve bars. Repeats, named parts and
song structure are what the player is trying to internalise, so they belong in
the format and clients should render them as structure. rifftext already has
this: `melody.Verse:PartA*2,PartB*2` and `structure:Intro,Verse,Chorus,…`.

**5. Prefer the general form when it is nearly free.** Two shape changes were
made while building the demos: chords went from a flat array to
groups-with-repeats, and lyrics from an implicit 8-beat grid to explicit start
times with gaps. Both would have broken clients in the wild; both cost about
one number to get right up front.

**6. The dumb client is the canary.** The terminal client has no JSON parser
ambitions, no audio, no layout engine, and 80 columns. If a change is awkward
for it, it is too clever. "Too clever" means non-obvious, not "any logic at
all" — five lines of modular arithmetic to resolve a repeat is fine.

## Notation: rifftext

`rifftext/` is the single music format. It already carries:

```
key:Am
tempo:110
tempo.Chorus:120
sig:4/4
rhythm:q,q,q,q                  # durations, incl. fractions: s/3 for triplets
melody.PartA:0,2,4e,5e          # chromatic intervals from the tonic
melody.Verse:PartA*2,PartB*2    # parts compose, and repeat
chords.Chorus:C,Am,F,G          # or diatonic: I,IV,V,iii
chord.A:[0,4,7]                 # chords can be defined
drums:K,S,K,S,Ks,Ks,S,K
structure:Intro,Verse,Chorus,Verse,Chorus,Outro
```

Three properties matter for the protocol:

- **melodies are intervals from a tonic**, so transposition is free and a key
  change is one directive, not a rewrite.
- **parts are named and composable**, with `*N` repeats — principle 4 for free.
- **it is already line-oriented text.** The small-client story needs no
  separate TSV encoding: a client that cannot parse JSON parses rifftext
  directly. That is a real convergence and worth protecting.

Consequence: pinning rifftext's grammar *is* the stability problem. The
`# new syntax` block in `rifftext/examples.riff` says it is still moving; it
should stop moving before clients ship against it.

## What rifftext does not cover

Three gaps. Each is a sidecar keyed to the notation, never a second copy of it.

### Lyrics

Not in `RiffIr.v3` at all. Syllables with their own timing, since a syllable
starts where it starts and not on the melody's grid:

```json
{ "lyrics": [
  { "beat": 4, "syllables": [
      { "beat": 4,   "dur": 1,   "text": "We" },
      { "beat": 5,   "dur": 1,   "text": "write" },
      { "beat": 6,   "dur": 0.5, "text": "the" },
      { "beat": 6.5, "dur": 1.5, "text": "loops" } ] },
  { "beat": 12, "syllables": [] }
] }
```

Syllable beats are absolute, not relative to the line — one rule instead of
two, and a syllable can be read without its parent. `join: true` glues a
syllable to the previous with no space, so `clean` + `join est` is one word
with two landing marks.

Note what is absent. A rest between lines is just the gap between one line's
last syllable and the next line's `beat`; there is no gap field. Whether that
gap earns a countdown, how many marks, when the next line moves up — client
policy, and the two existing clients already answer it differently. A sustain
is `dur` being long; there is no sustain flag.

Lyrics live in performance time and are *not* folded by `*N` repeats — verse 2
has different words over the same chords. This is the one place the repeat
structure and the words genuinely disagree, and writing lyrics out is how every
lead sheet on earth handles it.

### Realization: fingerings, tab, voicings

This is the real gap. Renderings divide by whether the instrument's realization
is *derivable* from the note:

| rendering | derivable? | needs |
|---|---|---|
| sheet music, piano roll | yes | pitch + duration |
| harmonica | mostly | harp key; bends are ambiguous |
| clarinet, recorder | yes | pitch, plus alternates |
| **guitar / bass tab** | **no** | a chosen string and fret |
| **chord voicing / shape** | **no** | a chosen shape |

The same pitch sits at up to six places on a guitar neck. Choosing is a musical
decision — position, open strings, voice leading, what the hand just left — and
no solver gets it right without knowing intent. So realization is an **optional
overlay**, attached to notation positions, that a renderer uses when present
and solves for when absent:

```json
{ "realization": {
    "instrument": "guitar",
    "tuning": "EADGBE",
    "capo": 0,
    "name": "as recorded",
    "notes":  [ { "part": "PartA", "step": 3, "string": 2, "fret": 5, "finger": 1 } ],
    "chords": [ { "part": "Chorus", "step": 0, "frets": [-1,3,2,0,1,0],
                  "fingers": [0,3,2,0,1,0] } ]
  } }
```

`-1` is a muted string, `0` open. MusicXML's `<technical><string>/<fret>` and
Guitar Pro both work this way; there is no need to invent a shape language.

Two consequences worth stating:

- **Overlays are plural and selectable.** "As recorded", "open position",
  "easy" are different overlays over identical notation. That is exactly the
  simplification the TODO asks for — a beginner's arrangement is not a
  different song, it is a different realization.
- **Absence is normal.** A client with no overlay renders tab from its own
  solver and preferences. The overlay improves the answer; it is never required
  for the rendering to exist.

### Strum direction

rifftext's `rhythm:` gives *when* the hand moves; it has no notion of *how*.
Direction and attack are a small overlay on the rhythm:

```json
{ "strum": [ { "beat": 0, "stroke": "d" }, { "beat": 1.5, "stroke": "u" },
             { "beat": 2.5, "stroke": "x" } ] }
```

`d` down, `u` up, `x` percussive hit, `m` mute. **Absence means nothing is
played** — a rest is not an event. Unknown stroke values are skipped.

## The score package

```json
{
  "riffhound": 1,
  "id": "sha256:9f2c…",
  "title": "A Way To Say",
  "artist": "",
  "notation": "songs/a-way-to-say.riff",
  "anchors": [],
  "lyrics": [],
  "realizations": [],
  "strum": []
}
```

`notation` references the rifftext file rather than embedding it: it stays
hand-editable, diffable, and usable by tools that know nothing about this
wrapper.

`anchors` is an optional list of `{beat, t}` (t in seconds) aligning the
notation to a recording that drifts against a metronome. A client that ignores
it still plays in time, it just slides against the audio — degraded, not
broken, which is the intended failure mode for anything optional.

Key, tempo, meter and structure are **not** repeated here. They are in the
notation, and duplicating them is how the two copies start disagreeing.

## Session protocol

Looser, and expected to change. Same server as the library.

```
→ hello    { client: "textui/0.1", caps: ["chords", "lyrics", "strum"] }
← state    { song: "sha256:…", beat: 41.5, at: 1738104000123, rate: 1.0,
             playing: true, tempo: 95 }
→ seek     { beat: 32 }
→ transport{ playing: false }
→ tempo    { bpm: 100 }
→ mark     { beat: 132, text: "learn this" }
```

**Position is `{beat, at, rate}`, not a bare beat.** `at` is a wall-clock stamp
and `rate` is beats per second, so a client extrapolates locally between
updates instead of needing a message per frame. That is what keeps four screens
in a garage agreeing without a chattering socket, and it degrades by drifting
slowly rather than freezing.

`caps` is how a client says which renderings it can draw, so the server can
skip what nobody will use. Negotiate capabilities; never sniff versions.

Open: who owns transport. The README argues that for friends in a room the rule
should be "anyone may change it, everyone sees who did" — no permission model,
just attribution on the message.

## Compatibility rules

Normative, because "we'll be careful" is not a mechanism.

- A reader **MUST** ignore unknown fields, unknown overlay kinds, unknown
  rifftext directives and unknown enum values. This is what lets a new server
  serve an old client, which is the whole game.
- A field **MUST NOT** be renamed, retyped, or repurposed. Adding an optional
  field is the only safe change.
- `riffhound: <int>` bumps only for a change that would make an older client
  *silently misread* data — never for additions.
- Absent and null mean the same: not specified, client supplies its default.
- Numbers are beats unless the field says otherwise; `t` is always seconds.

## Conformance

- `protocol/testdata/` holds golden songs covering the awkward cases: an
  off-beat chord change, a lyric line with a long rest before it, a part played
  three times with a different second ending, a realization overlay for an
  instrument the client does not know, and an unknown directive that must be
  skipped without complaint.
- Every client parses the goldens as part of its build. The terminal client
  parsing them is the real test — see principle 6.

## Open questions

- **First and second endings.** `PartA*2` cannot say "but the last time,
  finish differently", and that pattern is everywhere. Does rifftext grow
  endings, or does structure list the variant explicitly?
- **Bends and slides** are pitch *and* technique. Notation or realization? A
  harmonica bend is arguably notation; a guitar bend is arguably both.
- Does a realization overlay key on `(part, step)` as sketched, or on absolute
  beat? Part-relative survives edits to other parts; beat-relative is simpler
  to look up. Probably part-relative, with the index rebuilt on load.
- Multiple simultaneous chord parts — capo'd guitar and piano — implies parts
  need an instrument tag, not just a name.
- Where do per-user practice marks live? Not the score; per-person and
  per-song. Probably a separate document keyed by song id.
