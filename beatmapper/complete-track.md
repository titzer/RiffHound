# Complete Track

The Complete Track tool takes a partially mapped track and proposes the rest:
beats for the unmapped stretches, sections where the mapped ones repeat, and
chord charts for those sections.  Nothing is written to the map until the user
accepts a proposal; every proposal is previewed on the timeline as a ghost.

The premise is that a song repeats itself.  A user who has carefully mapped one
verse and one chorus has, in effect, labelled training data for the rest of
the track.  The tool lifts those mapped stretches, finds where they recur in
the audio, and lays them down again -- stretched gently to fit, aligned to the
onsets it can hear, and carrying the sections and chords that were on them.

## Source layout

| File | Role |
|---|---|
| `src/complete_algo.h/.cpp` | The three inference stages.  Pure computation, no ImGui. |
| `src/onset_shape.h/.cpp` | Onset timbre shapes: descriptors, vocabulary, the track-wide *rhythm map*. |
| `src/beat_chroma.h/.cpp` | Beat-synchronous chroma cache with attack discounting. |
| `src/ui_complete.cpp` | The dock tool: settings, candidate list, accept/reject. |
| `src/ui_rhythm.cpp` | The Rhythm Map dock tool (owns the shape parameters). |
| `src/ui_timeline.cpp` | Ghost rendering and the Timbre strip. |
| `src/bench.cpp` | `bmbench`, the headless regression tool (see `bmbench.md`). |

Everything in `complete_algo.cpp` runs from a `CompleteInputs` (map, audio,
region) and a `CompleteParams` (every knob) to a `CompleteProposal` (beats,
chords, ranked candidates).  The UI only marshals those.

## Features

Two per-beat descriptions of the audio underpin all three stages.

**Beat-synchronous chroma** (`beat_chroma`).  One 12-vector of pitch-class
energy per beat interval, computed over the *body* of the beat: the first
`max(60 ms, 15 %)` of each interval is skipped, because that is where the drum
hit's broadband noise sits.  The cache is keyed on interval bounds, so moving
one beat recomputes only the two intervals around it.  The default algorithm
is "HPS + Peaks", an average of the Harmonic Product Spectrum and Spectral
Peaks chroma.  This was chosen by benchmark: for per-beat chord recognition on
Nowhere Man it reaches 76 % where NNLS reaches 41 % and Goertzel ~30 %.

**Rhythm map** (`onset_shape`).  Chroma says *what* is playing; it says almost
nothing about *where the beat is*, because harmony changes slowly and a
half-beat shift of a verse template scores nearly as well as the right
placement.  What distinguishes beat 1 from the "and" of 1 is the timbre of the
attack -- kick, snare, hat, strum, nothing -- and the mapped beats are
labelled examples of exactly that.  So:

1. Every onset in the track is harvested (spectral-flux detector at a low
   threshold, in 20-second chunks).
2. For each onset a *timbre descriptor* is taken over the first quarter-beat
   after it: 24 log-spaced bands (40 Hz - 12 kHz) × 3 sub-frames, **minus the
   spectrum of the quarter-beat before the onset** (so it describes what the
   hit adds, not what was already ringing), band-weighted toward the kick and
   hat ends, unit length.
3. The descriptors of onsets inside mapped stretches are standardised and
   clustered with k-means++ (K = 5 by default, three restarts) into a
   track-specific *vocabulary of shapes*, ordered loudest first.  Every onset
   then gets a shape label and a soft membership.
4. A stretch of beats becomes a *rhythm pattern*: for each beat interval and
   each of 12 sub-beat slots, which shape hits there and how loud.  A per-beat
   *rhythm vector* (slots × shapes) sits alongside the chroma vector, and every
   "is this the same passage?" comparison blends the two.

The Rhythm Map tool shows the vocabulary (colour, hit count, level, and how
many hits of each shape fall on beats vs. half-beats); the Timbre strip draws
one rectangle per classified window, exactly as wide as the descriptor window,
numbered by shape.

## Stage 1: beats

**Gaps and templates.**  An interval longer than `gap_factor` (1.8) × the
median inter-beat interval is a gap, as are an unmapped head and tail.  With a
region selected only gaps intersecting it are filled, clipped to it (a border
within a beat of a real mapped beat keeps that beat as the anchor, so the fill
still lands on it).  Templates are the fully mapped sections, plus
unsectioned mapped runs cut into 16-beat chunks; each carries its beat
intervals, chroma and rhythm pattern.

**Gap-fill strategies.**  `BEAT_FILL_ALGOS` is a table of strategies, selected
in the tool's Beat settings; a strategy supplies a placement scorer and a
snap policy, the driver is shared.

- *Chroma transfer* scores a placement (template, start, stretch) by the mean
  cosine between the template's per-beat chroma and a chroma frame grid
  across the gap, times a coverage factor, minus `warp_weight` × |stretch - 1|.
- *Rhythm-shape transfer* adds `rhythm_weight` × a rhythm score:
  for each template slot that expects a hit, is there an onset of that shape
  in the corresponding slot of the gap (±1 slot)?  Matches add, missing
  expected hits and loud unexpected ones subtract.  The rhythm term is
  sharply phase-selective -- on a blues backing track it reads 0.43 at the
  true placement and ≤ 0.05 at every other 1/12-beat offset -- and is added
  as a bonus so a passage whose drum pattern differs from the template still
  transfers on chroma alone.  Its snap policy prefers onsets of the shape the
  slot expects, falling back to the nearest onset.
- *Anchor + region growing* (default) replaces the greedy left-to-right chain
  with many anchors grown outward (automation.md §5): template placements
  scored anywhere in the gap (phase-refined, kept by non-maximum
  suppression), periodicity-supported onsets seeding long template-less
  stretches, and the gap's mapped edges.  Regions grow beat by beat both
  ways -- onset-snapped, tempo leashed to the edges' periods -- and where
  two regions meet, the space is bridged and a bridge far from a whole
  number of beats is flagged `[grid break?]` (a real tempo discontinuity
  stays visible instead of dragging everything after it).  Several anchor
  *scenarios* -- including the plain greedy chain -- are run to complete
  outcomes and ranked by an outcome score (onset support weighted by the
  track's on-beat shape prior, template placement quality, tempo smoothness,
  half-beat flips, period sanity); the best finished result wins, so
  choosing a strategy is never committing to it.  On the 25-track library
  this recovers the greedy chain's wins verbatim and adds large gains where
  the chain drifted (Proud Mary 128 → 226 hits, Save Tonight 201 → 318,
  Margaritaville 272 → 350 on the drop-50 scenario).

**The greedy fill** (strategies 1-2, and one ranked candidate of strategy 3).
Left to right from the gap's left anchor.  At each
position the best (template × stretch ∈ ±8 %, 7 steps × start) is taken if
its score clears `beat_sim_threshold` (0.55); the template may start up to
`lookahead_beats` (4) whole beats ahead, paying `lookahead_penalty` per beat,
with the intervening beats tempo-filled -- this lets a 32-beat verse win over
a 14-beat riff that happens to fit right now.  Sub-beat start slack
(`jitter_beats`) exists but defaults to 0: it was the source of half-beat
seams.  Where nothing matches, one beat is laid along the seeded beat
detector's own grid (it follows the audio's tempo) and the search repeats.

After the pass: seam repair drops any beat that crowds its predecessor by less
than 0.6 of a beat (a tempo beat by preference); tempo runs bounded on both
sides are re-spaced evenly; each segment is **refit** -- the onset each beat
would snap to is found, the segment's stretch and offset are refit to those
onsets by least squares (the template's own spacing is only a prior), beats
are re-placed on that line and pulled `onset_weight` of the way onto their
onsets; then a light smoothing pass with the anchors pinned.  A second-stage
per-segment smoothing (`Smooth N selected segments`) can be run any number of
times from the tool, with its own knobs.

Each transferred segment or tempo run is one candidate, labelled with its
template, stretch, similarities and warp (stretch plus the post-snap local
deviation), so the list reads as segments the user can accept individually.

Honest status: on a backing-track loop this stage is essentially perfect
(151/151 hidden beats within 50 ms, 7.5 ms mean).  On a real song (Nowhere
Man) it is good on some hidden stretches (189/205) and poor on others: the
greedy chain means one wrong-phase placement poisons everything after it.  A
beam or DP search over placements with explicit phase verification is the
planned fix.

## Stage 2: sections

Sections are inferred from the beats *in the map*, so the workflow is: accept
beats, re-analyze.  Two sources, both using the chroma + rhythm blend
(`section_rhythm_weight`, 0.4) compared one measure at a time by default.

*Partition DP* (default).  Sections tile the track, so the uncovered spans
between known sections are not fields for independent sliding matches: each
span is inferred as a *sequence* of blocks that must meet the known edges
exactly.  A dynamic program over the span's beats chooses the sequence:
template blocks (every mapped section, compared measure-by-measure on
prefix-summed chroma + rhythm features), gently truncated or extended by
whole measures -- extra measures compare cyclically against the template's
last two, so a final chorus that repeats its tail matches as one long block
-- and per-beat filler for what matches nothing.  Each block costs a fixed
`section_block_penalty` (8), which is what keeps the DP from covering a
hidden chorus with confetti of small high-similarity fragments; blocks whose
similarity clears the DP's soft gate but not `section_sim_threshold` arrive
deselected.  Anchoring both ends makes the off-by-a-few-beats placements of a
sliding search structurally impossible for interior spans.  (A learned
kind-transition prior exists behind `section_prior_weight` but defaults to
off: with only a handful of known sections the bigram counts are too sparse
to help.)

On the 25-track library's leave-one-out benchmark (hide one instance of a
section kind that appears three times; its siblings remain) the partition DP
recovers 70 % of hidden sections against the sliding matcher's 59 %, with
22 % fewer false positives and the riding chords right on 84 % of beats
(77 % before).  Its known weakness is an *inferred* beat grid: blocks must
meet the span edges exactly, so beat-stage errors hurt it more than they
hurt sliding matches.

*Sliding template matching* (`section_partition=0`) remains as the fallback
path: placements clearing the threshold, local-maximum over ±1 beat, refined
±2 beats at beat granularity, with longer templates ranked first in overlap
suppression.

*Chords ride along.*  If the template section has chords, each chord is mapped
to the candidate at the same beat offsets and spot-checked: the chord's span
at the candidate must sound like it does in the template (duration-weighted
mean above `chord_sim_threshold`, 0.70).  If it does, the chords attach to the
section candidate and the two are accepted as one unit; the list shows
`+ 7 chords (check 0.87)`.  Spans owned by such a candidate are reserved from
every other chord source.

*Discovery.*  With `Discover repeats` on, blocks that nothing matches are
found by self-similarity, no template needed: a measure-aligned block is a
section unit when it is immediately followed by its own repeat (the smallest
such period, scanned shortest-first from `section_min_measures`); at each
uncovered position an existing unit is tried first (longest first), groups
that turn out mutually similar merge, and groups are labelled A, B, C in
time order, the most repeated called chorus.  On the blues loop this recovers
all 22 twelve-bar verses as one group; on Nowhere Man, three 8-bar verses, the
bridge halves, and two 16-bar solo blocks.

All section and chord edges are mapped to the **nearest** beat, not the first
beat at or after -- an edge sitting a few milliseconds past its beat used to
shift a whole template one beat late.

## Stage 3: chords

Three sources, in order of confidence; earlier ones reserve their spans.

1. *Section transfer.*  A mapped section with no chords borrows the chart of
   the most similar mapped section of the same kind that has one, offsets
   travelling in beats.
2. *Progression repeats.*  Each run of consecutive mapped chords is slid
   across the chord-free grid; a placement scores by how much each chord's
   span sounds (chroma + rhythm) like it did in the map.
3. *Decoder.*  Beat by beat where nothing else applies.  The vocabulary is the
   map's own chords, each modelled as the average chroma of its spans in this
   track blended with its textbook triad (`chord_prior_beats` pseudo-beats of
   triad, so a chord seen once is not over-fitted); unseen triads are offered
   at a penalty.  Viterbi over the beats with a cost for changing chord
   (`chord_transition`), half waived on measure starts, and scaled by the
   map's median chord length so a song that changes every two beats is not
   forced into four-beat chords.  Proposals start deselected.

With beats and sections known, hidden chords come back right on 92 % of beats
for Nowhere Man, 88 % for Margaritaville and 99 % for the blues loop.

## The UI

Complete Track lives in the right-hand dock (Tools menu, or its rail icon).
Like every dock tool it is a framed box with the settings behind the triangle
in its header, the body in the middle and the action buttons at the bottom.

- **Body**: the Beats / Sections / Chords toggles, the status line, a
  `Ghosts` toggle, and the candidate list grouped by stage.  Each row has a
  tick box, a score (green ≥ 0.75, amber ≥ 0.5, red below), the description,
  and an **✕** that removes just that suggestion.  Hovering a row highlights
  its ghost; a click scrolls it into view; a double-click toggles it.
- **Ghosts** on the timeline: hollow diamonds with a span bracket for beats,
  dashed boxes for sections, outlined boxes for chords (including the chords
  riding with a section).  Three levels of presence: faint when deselected,
  solid when ticked, bright orange under the mouse.
- **Region narrowing**: with a region selected, Analyze fills only gaps inside
  it, and the list shows only candidates that substantially intersect it
  (half of the candidate inside, or the region inside the candidate).  The
  intended workflow is to analyze the whole track, then select the part that
  came out right and accept just that.
- **Actions**: `Analyze track` / `Analyze region`; `Select all`,
  `Deselect all` (then tick one at a time), `Discard`; `Smooth N selected
  segments` (second-stage smoothing, repeatable); `Accept N selected`, which
  inserts everything ticked as one undo entry.  Accepting beats then
  re-analyzing is how sections and chords are reached.
- **Settings**: Beat settings (strategy, rhythm bonus, stretch range and
  penalty, lookahead, thresholds, the onset detector, fill and second-stage
  smoothing), Section settings (granularity, threshold, overlap, rhythm
  weight, discovery), Chord settings (progression repeats, decoder), and
  Chroma per beat (algorithm, attack skip).  The timbre-shape parameters live
  in the Rhythm Map tool and are shared.

The proposal persists until discarded, a new track is loaded, or it is
replaced by the next Analyze.
