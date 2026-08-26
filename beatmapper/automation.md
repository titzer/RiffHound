# Toward an automatic beatmapper

Notes on where the accuracy work goes next.  The goal is that a new track
needs minutes of correction, not an evening of annotation: the tools propose,
the human accepts.  Everything below is in service of that inversion.

Where we use standard MIR terminology, it is deliberate: beat tracking,
downbeat estimation, structural segmentation, chord recognition, onset
detection, self-similarity matrix, tempogram.  Project-specific names
(the Complete Track tool, the onset-timbre vocabulary behind the Rhythm Map)
are glossed on first use.

The current state, for context: beat gap-filling transfers mapped stretches
by chroma + onset-timbre similarity with a greedy left-to-right decode over a
detector grid; sections are inferred by a partition dynamic program over
known edges (leave-one-out recovery 70% on the 25-track library, up from 59%
for the sliding matcher); chords ride on sections with a per-chord spot
check, plus a Viterbi decoder over learned chord templates (92% chord-beat
accuracy on a well-mapped track).  The weakest links, in order: beat
tracking over long unannotated stretches (tempo drift, boundary errors),
section label confusion where harmony repeats, and every stage degrading
when it runs on an estimated rather than a hand-checked beat grid.

## 1. The evaluation harness is the engine

Every idea below is only as good as its measured delta, so the evaluation
harness (bmbench) is not a side tool -- it is the development loop.  As the
library of fully annotated tracks grows, it becomes the dataset.

- **Leave-one-out** (`sect/loo` today): hide one instance of a repeated
  element, infer it back.  Extend beyond sections: leave-one-out for chord
  progressions (hide one chorus's chords) and for beat stretches (hide one
  verse's beats with everything else intact).  This measures the *transfer*
  machinery in isolation.
- **Layer ablation** ("leave-all-out"): hide a whole annotation layer (all
  sections, all chords, beats past t=60) and rebuild it from the rest.
  This measures the *pipeline*, including error propagation across stages
  -- exactly where the current system loses the most (the all/* suite rows).
- **Cold start**: infer from audio alone, no annotations.  Today this barely
  works; it should become a tracked number rather than an aspiration, so
  progress toward true automation is visible.
- **Ground truth is noisy.**  The annotations are good enough to treat as
  reference, but they carry their own error (hand-placed edges, the odd
  mislabeled chord).  Metrics should stay tolerance-based (50ms beat hits,
  0.25s section edges -- in line with MIREX-style evaluation windows),
  report medians alongside means, and any single-track anomaly should
  prompt a look at the annotation before a look at the algorithm
  (cf. Highway to Hell's chords, Folsom's partially annotated sections).
- **Regression discipline**: keep a checked-in TSV of the suite per
  algorithm change (bench/results-*.tsv), so a tuning win on one track that
  costs three others is caught the day it happens.

## 2. ML models as pluggable scorers, not oracles

The parameter plumbing was built so every stage is configurable; the same
boundary is where ML slots in.  The realistic wins are small, task-specific
models -- full automatic-transcription models (MT3, Basic Pitch) are worth a
benchmark run as an upper-bound reference, but the interesting integrations
are:

- **Beat and downbeat tracking**: TCN/RNN beat trackers of the madmom
  family (or Hugging Face ports such as Beat-Transformer).  Use the
  *beat activation function* -- the model's per-frame beat probability --
  not its decoded beat sequence: feed the activations into our own dynamic
  program as an observation term alongside spectral flux.  That keeps our
  editing semantics (anchors, region restriction) and treats the model as a
  better onset front end.
- **Chord recognition**: a small CRNN chord model as an alternative
  observation source -- the CHROMA_ALGOS table already makes front ends
  selectable, and the chord decoder already blends observation sources.
  The benchmark showed the chroma front end dominates chord accuracy
  (HPS+Peaks 76% vs NNLS 41% on one track); a learned front end is the
  natural next rung.
- **Structural-segmentation embeddings**: per-beat embeddings from a music
  structure model as a third feature stream next to chroma and the
  onset-timbre pattern vectors in the section DP (a learned similarity
  blended with, or replacing, cosine-on-chroma).
- **Custom micro-models**: the onset-timbre vocabulary is a hand-rolled
  k-means classifier; the same descriptor windows could train a small
  self-supervised embedding on our own annotated library.  Tiny (thousands
  of parameters), exportable to a flat file -- no Python at runtime.

Practical shape: an external-scorer interface.  A stage asks for
`score(track, t0, t1)` or `activations(track)`; a subprocess (Python, ONNX
Runtime, whatever) answers over a simple file/pipe protocol; bmbench can run
the same scorer headlessly.  Cache activations per track on disk so the cost
is paid once.  Every scorer competes in the harness against the DSP baseline
before it earns a default slot.

## 3. New DSP features, validated in the harness

The onset-timbre classification earned its place everywhere (beat filling,
section similarity, chord-progression matching).  More candidate features in
the same spirit -- cheap, track-specific, learned from the annotated
portion:

- **Bass-line contour**: fundamental of the lowest voiced register per beat.
  Distinguishes verse from chorus when the chords match but the bass line
  differs; also a strong downbeat cue.
- **Tempogram**: local autocorrelation of the onset-strength envelope as a
  per-window tempo distribution.  Gives beat tracking a confidence signal
  (sharp peak = trustworthy region, flat = quiet or rubato) -- this feeds
  the anchor selection in §5 directly.
- **Section-scale energy envelope**: sections differ in RMS and spectral
  centroid long before you look at pitch.  A 3-band, beat-synchronous
  envelope is nearly free and helps the coarse segmentation pass in §4.
- **Stereo width**: doubled guitars in choruses vs dry verses; another cheap
  discriminator for the same-harmony label-confusion failure.
- **Harmonic-percussive source separation (HPSS)** as a preprocessor: run
  onset detection on the percussive component and chroma on the harmonic
  component, instead of both on the mix.  Likely helps both ends at once.

Rule: every new feature lands as (a) a column in the beat-synchronous
feature prefix sums so the section DP can blend it, (b) an optional term in
the beat-fill score, and (c) a `--set` weight in bmbench so its value is
measured, not assumed.  The blend weights themselves are then §6's problem.

## 4. Hierarchical structure: coarse-to-fine segmentation

Songs are hierarchical -- song / section / phrase / measure / beat -- and
the current pipeline works almost entirely at the measure/beat level.  A
coarse pass should come first, in the standard coarse-to-fine pattern:

- **Smoothed self-similarity matrix**: compute the SSM over heavily
  smoothed beat-synchronous features (2-4s windows) and segment *that* into
  a rough A/B/C form (novelty-based boundary detection plus repetition
  grouping).  The low resolution is a feature, not a compromise: tempo
  drift and beat-grid errors wash out, so the rough boundaries stay robust
  even on an estimated grid -- exactly the regime where the current
  partition DP degrades.
- **Then refine downward**: each rough boundary is re-localized at measure
  granularity within a +/-1-measure window, then at beat granularity, then
  snapped to the strongest nearby onset.  Each level searches only near its
  parent's answer, so the fine search space collapses.
- **Form as a prior**: the coarse A/B/A/B/C/B form string constrains the
  fine pass.  (The corpus-level label-transition bigram measured worse as a
  flat prior, but "this span repeats bars 20-36" is a per-track statement
  and far stronger.)
- The phrase level (4/8-measure groups) sits between section and measure
  and is where lyric alignment and "a verse is 16 bars" constraints
  naturally attach.

This also gives the UI something honest to show early: rough section guesses
appear seconds after load and sharpen as the fine passes finish (they can
run on the background worker like the Complete Track analysis now does).

## 5. Beat tracking: anchor beats and region growing

*Status: first implementation shipped as the Complete Track tool's default
gap-fill strategy ("Anchor + region growing"); the greedy chain competes as
one of the ranked outcome candidates.  See complete-track.md.*

The single most important fix.  The greedy left-to-right decode is the known
weakness: one bad handoff and everything after it drifts (documented in the
bench results: 189/205 hits on one hidden stretch, 117/246 on another, same
parameters).  The redesign replaces "one chain" with "many anchors" --
region growing from high-confidence anchor beats, in the spirit of
confidence-based beat trackers that start from reliable islands rather than
the start of the track:

- **Anchor selection**: find high-confidence beats across the whole track
  first -- strong isolated onsets that agree with the local tempogram peak,
  hits classified as a trained drum timbre, spans where transferred
  templates score far above threshold.  Confidence is explicit and kept.
- **Region growing**: extend the beat grid outward from every anchor
  simultaneously, beat by beat, each extension snapping to onsets and
  penalized for tempo deviation.  Growth stops where confidence falls below
  a floor (a quiet bridge, rubato, a splice point) instead of plowing
  through.
- **Boundary reconciliation**: where two grown regions meet, either they
  agree within tolerance (merge, distributing the residual over the last few
  beats -- the existing seam-repair pass, generalized) or they disagree,
  which is *information*: a tempo change, a dropped half-beat, or an edit
  point.  Disagreements become explicit UI items ("the grid breaks here"),
  because tracks assembled from samples genuinely contain metrical
  discontinuities, and modeling the grid as continuous is where the drag
  comes from.
- **Global decode**: after growing, a track-level dynamic program over the
  region-boundary graph decides which regions to trust where -- this is the
  beam/DP search the docs already list as the open item, but run over
  region boundaries (dozens) instead of every beat (thousands), so it stays
  cheap.
- **Drift control**: within a region, the existing least-squares refit
  against snapped onsets becomes a local tempo model; long low-confidence
  spans are bridged by interpolating from *both* neighboring regions toward
  the middle rather than extrapolating from the left only -- half the
  accumulated drift by construction.

The Complete Track transfer machinery stays: a transferred template is
simply an anchor region with high confidence over its whole span.

## 6. Automatic parameter tuning in the harness

Every important knob is already reachable (`--set` covers all of
CompleteParams via the PARAMS table).  What is missing is the outer
optimization loop:

- A tuner driver that runs the suite over a parameter set and optimizes:
  per-parameter sweeps first (cheap, parallelizable one process per track),
  then coordinate descent or random search over the top movers.  The
  objective is a weighted sum over scenario metrics (beat F-measure-style
  hits, section recovery minus false positives, chord-beat accuracy) --
  write the objective down once so tuning runs are comparable.
- **Guard against overfitting the library**: cross-validate with held-out
  tracks (split by track, never by scenario), and prefer parameter settings
  that are flat near the optimum over sharp peaks.
- **Per-regime presets**: the right lookahead for a steady rock track is
  wrong for a rubato ballad.  Cheap track statistics (tempo variance, onset
  density, spectral flatness) can select among a few tuned presets -- tune
  the presets in the harness, select automatically at runtime.
- Re-run the tuner whenever the library grows by a few tracks; keep the
  tuned defaults and their provenance (library size, date, objective value)
  in a checked-in file so `complete_params_defaults` has a paper trail.

## 7. Bulk editing: group, paste, align

Manual correction speed matters as much as inference accuracy -- the human
pass is part of the system.  The clipboard now handles multi-select
copy/paste per strip; the next level is cross-layer and alignment-aware:

- **Cross-layer groups**: select a time range and grab *everything* in it
  -- beats, sections, chords, lyrics -- as one group (the section
  candidate's "chords ride along" pattern, generalized to the editor).
- **Alignment-aware paste**: on paste, don't place at literal time offsets.
  Anchor the group's beats to the target the way template transfer does:
  time-stretch uniformly to fit the target span (or the gap between the
  surrounding beats), snap each beat to a nearby onset, then map the riding
  sections/chords/lyrics through the same time warp
  (`beatmap_retime_annotations` already implements exactly this mapping for
  smoothing -- reuse it).  A paste then means "this verse happens again
  here", not "these timestamps minus 83.2 seconds".  If uniform stretching
  proves too rigid, the general tool is dynamic time warping between the
  source and target feature sequences.
- **Preview like the Complete Track tool**: ghosts at the paste target with
  the alignment applied, accept/adjust before committing; score the fit
  with the same chroma + onset-timbre similarity so a bad placement is
  visibly bad.
- **Named templates**: a copied group is a reusable template; a small
  per-track palette of them ("verse", "chorus riff") turns mapping a
  repetitive song into painting.  This is also exactly the data structure
  the Complete Track template transfer wants, so the two share code and the
  manual palette can seed the automatic search.

## Order of attack

1. Anchor-based beat tracking (§5) -- it unblocks everything else, since
   all downstream stages degrade on a drifting grid, and the all/*
   scenarios are the ones that measure real automation.
2. Harness extensions + tuner (§1, §6) -- cheap, and every later change
   needs them to prove itself.
3. Coarse-to-fine segmentation (§4) with one or two new features from §3
   aimed at the section label-confusion failure.
4. External-scorer interface + first ML baseline (§2) -- beat activations
   first, since that is where the ceiling is most obviously above the DSP.
5. Alignment-aware bulk editing (§7), sharing its template machinery with
   the Complete Track tool.
