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

Section metrics: hidden sections recovered (both edges within 0.25 s and
the same kind), spurious proposals inside the truth range, and `sec_near`:
misses whose nearest same-kind proposal has both edges within a beat -- the
count to watch for phase slips.  With `--list`, every miss prints its
nearest proposal and the signed edge errors in beats.

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

### `chroma`

Per-beat chroma with the truth chord: one TSV line per truth beat interval
(track, index, t0, t1, chord token, twelve values).  Works on a directory.
This is the training set for `scripts/train_chords.py`, which fits the
corpus chord emission model in `src/chord_model.h` (see below).

### `params`

List every settable parameter with its default and a one-line description.
Names are the `CompleteParams` field names, with nested ones dotted:
`smooth2.strength`, `chroma.algo_idx`, `shape.k`, ...

### `suite`

The systematic scenario matrix; run it on a track or on a directory of
track/`.txt` pairs (e.g. `bench/tracks-anno`, symlinks to the well-annotated
tracks in `~/playalong` and `~/backing_tracks`).

```
bmbench bench/tracks-anno suite --tsv --header
bmbench bench/tracks-anno suite --only all/cold
bmbench bench/tracks-anno suite --only sect/loo      # leave-one-section-out
bmbench bench/tracks-anno suite --only all/loso      # leave-WHOLE-section-out
```

Rows: `beats/tail-50`, `beats/mid-25`, `beats/chroma-alt`, `sect/discover`,
`sect/tail-50`, `chords/tail-50`, `chords/decoder`, `all/keepsec0`,
`all/tail-60`, `all/chords-first`, and `all/cold` (nothing kept at all: the
detector finds tempo and phase, discovery finds the sections, the triad /
external decoder the chords).  Two aggregate modes run one scenario per
section and print one summed row: `sect/loo` hides one section instance
(sections + chords; beats stay) for every section whose kind appears three or
more times, and `all/loso` hides *everything* over each section of at least 8
beats -- the section must be recovered on re-inferred beats.

### External chord models

`--set chord_external=1` adds an out-of-process chord recogniser to the chord
stage.  One-time setup: `make chord-models` (or
`scripts/setup-chord-models.sh [madmom|btc] [--force|--check]`), which builds
the Python environments under `external/` and verifies them.  The command is `$BM_CHORD_CMD <audio>` when set, else
`external/venv/bin/python scripts/chords_madmom.py <audio>` (madmom CNN+CRF,
~83 % maj/min on Isophonics; `scripts/chords_btc.py` in `external/venv-btc`
is the BTC large-vocabulary alternative and also emits 7th/sus chords).
Output is one `start<TAB>end<TAB>label` line per chord; both scripts cache
per track under `~/.cache/beatmapper/chords/`, so only the first run pays the
model.  Model chords enter the Viterbi decoder as per-beat emission bonuses
(`chord_external_blend`, 0.1 with in-song models; `chord_external_blend_cold`,
0.25 when the map has no chords), scaled by an optional fourth column of
confidence.  `chord_external_tool` picks the recogniser: 0 madmom, 1 the
ensemble (`scripts/chords_ensemble.py`: every installed model, confident
where they agree; the default), 2 BTC.  `chord_external_blend=0` restores
span filling.

Two more emission terms need no external process.  `chord_corpus_weight_cold`
(0.2; `chord_corpus_weight` 0 with in-song models) mixes in the corpus-trained
model from `src/chord_model.h`: a transposition-tied softmax over the 24
triads, fitted by `scripts/train_chords.py` on the `chroma` dump of the
mapped tracks (66.5 % per-beat leave-one-track-out against the hand triad's
57 %, with the neighbouring beats' chroma as context).  Retrain after
mapping more tracks:

```sh
./bmbench bench/tracks-anno chroma > chroma.tsv
external/venv/bin/python scripts/train_chords.py chroma.tsv --context --no-eval --header src/chord_model.h
```

`chord_key_bonus` (0.05) favours unseen triads diatonic to the song's key,
estimated from its mapped chords or, cold, by Krumhansl profile correlation.

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

TSV columns, in order: `track scenario hidden n hit50 hit90 half missed
mean_ms max_ms sec_found sec_true sec_false sec_near cb_ok cb_n`.

`scripts/bench-par.py BINARY DIR ONLY [args]` runs one process per track in
parallel and concatenates the rows (a full `sect/loo` pass over the 18
tracks takes ~30 s instead of 2.5 min); `--list` lines come out on stderr
prefixed with the track.  Snapshot the binary first (`cp bmbench bmbench-tN`)
so a rebuild mid-sweep does not change what later tracks run.

## What it has found so far

- The chroma algorithm dominates chord accuracy: HPS + Peaks 76 % vs NNLS
  41 % on Nowhere Man; decoder knobs move it a few points.  The whitened
  log-frequency chroma (Cho & Bello front end) averaged with HPS + Peaks
  ("Whitened + HPS/Peaks", now the default) took the decoder from 76 % to
  83 % of chord beats over the 18-track set.
- Sub-beat start slack in the beat fill was the source of half-beat seams
  (15 → 1 on one scenario with `jitter_beats=0`).
- Section/chord edges mapped to the first beat at-or-after the edge shifted
  templates a beat late; nearest-beat snapping fixed it (`--edge-ms`).
- Beat completion on real songs is brittle to the greedy chain: the same
  parameters give 189/205 hits on one hidden stretch and 117/246 on another.
- The detector's tempo autocorrelation carried a DC pedestal that flattened
  its peaks; de-meaned harmonic-sum scoring with a mild log-Gaussian prior
  and a low-band (kick/bass) term fixed the tempo estimate on 17/18 tracks,
  and cold-start beats went from 46 % to 90 % across the set.
- The Ellis DP's tightness was in raw flux units, so its meaning depended on
  the recording level; the ODF is normalised to unit variance now and the
  default tightness is 50 (was 400).
- Two half-beat-shift generators in seeded fills: the seed-phase cosine bias
  extrapolated minutes past the mapped region (now tapered with distance to
  the nearest seed), and the least-squares refit flattening long tempo-fill
  segments into one constant-tempo line (long segments now refit on their
  own tracked spacing).  keepsec0 worst cases went from 28-54 % to 93-100 %.
- Template self-confidence could outvote audio evidence in the outcome
  ranking (0.35 weight on template quality vs a 0.27 onset-support deficit,
  Hold My Hand); the weight is 0.12 now and the detector's raw grid competes
  as its own ranked outcome.
- Discovery locked onto 4-measure sub-repeats of 12-bar forms; a unit is now
  the shortest length whose consecutive repeats resemble each other clearly
  more than the unit's own halves do (BBKing cold sections 0/4 → 4/4).
- The song's own section-kind bigrams break verse-vs-solo ties in the
  partition DP: `section_prior_weight=0.5` (now the default) took LOO
  section recovery from 119 to 123 of 153 (Hotel California 4/11 → 7/11).
- External learned chord models beat the triad decoder cold (madmom CNN+CRF
  62 % vs 53 % of chord beats with nothing annotated) but do not improve on
  within-song transfer once a few sections carry chords.  Known cases where
  the model and the annotator disagree on major-vs-minor (Country-E-188) or
  on the tuning reference of an off-pitch recording (BTC on Mary Jane's
  Last Dance) score as misses.

- Section proposals slipping a whole beat late in chains: the partition DP
  let filler advance one beat while measure-granularity similarity could not
  tell the phases apart.  Locking block starts to the mapped sections'
  measure phase (`section_phase_lock`) took sect/tail-50 from 41 to 57 of
  110 with the one-beat near-misses going from 16 to 0.
- Right edges, wrong kind was the rest of the leave-one-out misses: a
  spectral-balance profile per beat (`section_timbre_weight` 0.45, 24
  bands) took sect/loo from 122 to 127 of 153 and false from 49 to 43;
  all/loso 134 -> 137.
- Cold chords without an external model: corpus emission model + key bonus,
  58.2 -> 64.9 % of chord beats.  With the external ensemble, 67.6 %
  (madmom alone 67.1, BTC alone 65.9); the ensemble also edges the decoder
  scenario, 84.6 -> 85.0 %.  Blending external labels as emissions rather
  than filling spans is what keeps the in-song models in charge (span
  filling scored 69 % where the decoder alone scores 83 %).

## Known-hard cases

- `Cripple Creek 110 BPM in A` keepsec0/cold: the truth beats sit on the
  quiet side of the banjo pattern; every audio-driven phase choice prefers
  the loud offbeats.  Only the seed phase knows better, and extending its
  reach breaks tempo-drifting tracks that need the opposite.
- `I Cant Explain` cold: tempo estimate halves (prior tips it at 69 vs 139).
- Cold-start section discovery on verse/chorus material with weak harmonic
  contrast still over- and under-segments; edges land on measures but the
  unit boundaries are subjective.

## Adding a scenario or a metric

Hiding is a small list of `Hide` records applied to the loaded map in
`main()`; a new kind is one more `case`.  Scoring functions (`score_beats`,
`score_chord_beats`) take the truth and a proposal and return counts, so a
new metric is a function plus a column.  Parameters are registered in the
`PARAMS[]` table with a `PF`/`PI`/`PB` macro -- add a field to `CompleteParams`
and one line there, and it is sweepable.
