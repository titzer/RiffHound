# bmbench

`bmbench` is a headless regression tool for beatmapper's inference
algorithms.  It takes a fully mapped track as ground truth, hides part of the
map, runs a tool over what is left, and scores the proposals against what was
hidden.  Every parameter of the Complete Track tool is settable from the
command line and results can be printed as one TSV line per scenario, so an
outer script can sweep parameters or run a fixed scenario set as a regression
suite.

Build with `make bench`; the binary is `bmbench` in the beatmapper directory
(git-ignored).  It links the algorithm objects only -- no ImGui, no GLFW -- so
it also serves as a check that `complete_algo`, `onset_shape` and
`beat_chroma` stay free of UI dependencies.

```
bmbench <audio> [--map FILE] <command> [options]
```

The map defaults to the audio's companion `.txt`.  Audio is decoded to mono
with the same decoder the app uses.

## Commands

### `complete`

Hide part of the map, re-infer it with Complete Track, score.

```
bmbench "Nowhere Man.mp3" complete --drop 60-999 --smooth2 1 --stages
```

Hiding options, applied in order:

| Option | Effect |
|---|---|
| `--keep T0-T1` | keep only beats, sections and chords inside (the first `--keep` drops everything else; repeat to keep several ranges) |
| `--drop T0-T1` | drop beats, sections and chords inside |
| `--keep-section N` | keep only the N-th section (0-based) and what is in it |
| `--drop-beats N:M` | drop every N-th beat starting at M (thins the map; note that dropping every other beat doubles the median interval, so it is not a gap scenario) |
| `--no-sections` | drop every section, keep beats and chords |
| `--no-chords` | drop every chord |
| `--hide-chords T0-T1` | drop chords starting inside; beats and sections stay |
| `--edge-ms MS` | shift every visible section/chord edge by MS (late if > 0), simulating hand-placed edges that sit after their beat |
| `--region T0-T1` | pass a region to the tool, as the UI would |

Run options:

| Option | Effect |
|---|---|
| `--algo NAME\|IDX` | gap-fill strategy (`"Chroma transfer"`, `"Rhythm-shape transfer"`, or index) |
| `--set NAME=VALUE` | any parameter (repeatable; see `params`) |
| `--smooth2 N` | run N passes of second-stage per-segment smoothing on the proposed beats |
| `--stages` | after the beat stage, accept every proposed beat, run the section stage and score it, accept every proposed section (with its attached chords), run the chord stage and score it |
| `--tol MS` | tolerance for a beat "hit" (default 50) |
| `--list` | print every candidate, with per-segment accuracy for beat candidates |
| `--dump` | print every proposed beat with its offset from the nearest truth beat and its confidence |
| `--debug` | strategy trace on stderr (same as `COMPLETE_DEBUG=1`) |
| `--tsv` / `--header` | one machine-readable line (with a header line) |

Scores are limited to the ground-truth range (first to last truth beat);
proposals beyond it are unknowable and ignored.

Beat metrics: `n` proposed in range, `hit` within tolerance of a truth beat,
`bad` not, `half` within 0.3-0.7 of a beat of the nearest truth beat (the
phase-error statistic the chroma-only strategy was bad at), `missed` hidden
truth beats with no proposal within tolerance, mean and max error, and the
number of template transfers vs. tempo runs.

Section metrics (`--stages`): hidden sections recovered (both edges within
0.25 s and the same kind), and spurious proposals inside the truth range.

Chord metrics (`--stages`): chords proposed for hidden ones, chords with the
right name at the right time (start within 0.15 s), and **chord beats** --
of the truth beats whose chord was hidden, how many are covered by a proposal
naming the same chord (first token of the annotation, so `"D D D D"` is `D`).
This counts chords that rode along with accepted sections as well as the chord
stage's own, and is the number to watch for chord inference.

### `detect`

Run the Beat Detector over a range and score it against the truth beats.

```
bmbench Blues-Bm-BBKing.mp3 detect --region 40-70 --set det_threshold=1.2
```

### `shapes`

Train the onset-shape vocabulary and report, per shape, the hit count, level,
mean membership and how many hits fall on beats, on half-beats, or elsewhere
relative to the truth beats.  A shape with many on-beat hits and few
half-beat hits is what the rhythm-shape strategy keys on.

### `params`

List every settable parameter with its default and a one-line description.
Names are the `CompleteParams` field names, with nested ones dotted:
`smooth2.strength`, `chroma.algo_idx`, `shape.k`, ...

## Scenarios that have been useful

```sh
B=./bmbench
cd ~/RiffHound/backing_tracks
$B Blues-Bm-BBKing.mp3 complete --keep-section 0 --smooth2 1 --stages   # one verse kept of a 12-bar loop
$B Blues-Bm-BBKing.mp3 complete --no-sections --stages --list            # section discovery from nothing

cd ~/RiffHound/playalong
$B "Nowhere Man.mp3" complete --drop 60-999 --smooth2 1 --stages         # everything hidden after 60 s
$B "Nowhere Man.mp3" complete --drop 40-999 --list                       # the brittle case: read the per-segment lines
$B "Nowhere Man.mp3" complete --hide-chords 60-999 --set do_beats=0 --stages   # chords only, beats+sections known
$B "Nowhere Man.mp3" complete --no-sections --set section_discover=0 --set chord_runs=0 \
   --hide-chords 80-999 --set do_beats=0 --stages                        # the chord decoder alone
$B "Nowhere Man.mp3" complete --drop 60-999 --edge-ms 40 --stages        # late edges
```

A parameter sweep is a shell loop over `--set` with `--tsv`, cutting the
columns of interest:

```sh
for t in 0.05 0.1 0.2 0.4; do
  printf "transition=%s\t" $t
  $B "Nowhere Man.mp3" complete --hide-chords 80-999 --set do_beats=0 \
     --set chord_transition=$t --stages --tsv 2>/dev/null | cut -f18,19
done
```

TSV columns, in order: `algo hidden n hit bad half missed mean_ms max_ms
transfers tempo_runs sec_found sec_true sec_false ch_found ch_true ch_text_ok
cb_ok cb_n`.

## What it has found so far

- The chroma algorithm dominates chord accuracy: HPS + Peaks 76 % vs NNLS
  41 % on Nowhere Man; decoder knobs move it a few points.
- Sub-beat start slack in the beat fill was the source of half-beat seams
  (15 → 1 on one scenario with `jitter_beats=0`).
- Section/chord edges mapped to the first beat at-or-after the edge shifted
  templates a beat late; nearest-beat snapping fixed it (`--edge-ms`).
- Beat completion on real songs is brittle to the greedy chain: the same
  parameters give 189/205 hits on one hidden stretch and 117/246 on another.

## Adding a scenario or a metric

Hiding is a small list of `Hide` records applied to the loaded map in
`main()`; a new kind is one more `case`.  Scoring functions (`score_beats`,
`score_chord_beats`) take the truth and a proposal and return counts, so a
new metric is a function plus a column.  Parameters are registered in the
`PARAMS[]` table with a `PF`/`PI`/`PB` macro -- add a field to `CompleteParams`
and one line there, and it is sweepable.
