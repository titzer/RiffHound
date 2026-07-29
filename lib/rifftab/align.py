#!/usr/bin/env python3
"""Align a Guitar Pro tab to a track's beatmap using the audio itself.

A tab is written in musical time and says nothing about where the recording
starts.  A beatmap is in physical time and says nothing about harmony.  This
tool finds the mapping between them by correlating the tab's implied pitch-class
content against the measured chroma of the audio, searching over:

  * beat offset      -- where measure 1 beat 1 lands in the beatmap
  * transposition    -- the tab may be written for a different tuning than the
                        recording, or the recording may be off-pitch

Both fall out of the same search, and the score curve is the confidence: a sharp
single peak means the tab matches this recording, a flat curve means it does not
(a different arrangement, a live version, or the wrong song entirely).
"""

import argparse
import os
import subprocess
import sys
import math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpdump

RIFFDSP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "riffdsp", "riffdsp")


def read_beatmap(path):
    ts = []
    for line in open(path):
        line = line.split("#")[0].strip()
        if not line:
            continue
        p = line.split()
        if len(p) >= 3 and p[2] == "B":
            ts.append(float(p[0]))
    ts.sort()
    return ts


def audio_chroma(audio, beatmap_path, algo="nnls"):
    """One 12-vector per beat interval, via riffdsp."""
    out = subprocess.run(
        [RIFFDSP, "chroma", "--beats", beatmap_path, "--algo", algo, audio],
        capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError("riffdsp chroma failed: " + out.stderr[:300])
    rows = []
    for line in out.stdout.splitlines():
        if line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) < 15:
            continue
        rows.append([float(x) for x in f[3:15]])
    return rows


def tab_pcv(gpfile, tracks, per_beat_count):
    """Pitch-class vector per integer gp_beat, summed over melodic tracks."""
    pcv = [[0.0] * 12 for _ in range(per_beat_count)]
    for t in tracks:
        if t.percussion or not t.tuning:
            continue
        beats, measures = gpdump.read_beats(gpfile, t.number)
        onsets = [b for b in beats if not b.rest and b.notes]
        for i, b in enumerate(onsets):
            end = onsets[i + 1].gp_beat if i + 1 < len(onsets) else b.gp_beat + 1.0
            for (s, f, flags) in b.notes:
                if "x" in flags:
                    continue
                p = t.pitch(s, f)
                if p is None:
                    continue
                # Spread the note across every beat bucket it sounds through.
                lo, hi = b.gp_beat, min(end, b.gp_beat + 8.0)
                k = int(math.floor(lo))
                while k < hi and k < per_beat_count:
                    if k >= 0:
                        overlap = min(hi, k + 1) - max(lo, k)
                        if overlap > 0:
                            pcv[k][p % 12] += overlap
                    k += 1
    return pcv


def norm(v):
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v] if n > 0 else None


def cosine(a, b):
    return sum(x * y for x, y in zip(a, b))


def score_alignment(tabv, audv, offset, transpose):
    """Mean cosine similarity over beats where both sides have content."""
    tot, n = 0.0, 0
    for i, tv in enumerate(tabv):
        j = i + offset
        if j < 0 or j >= len(audv):
            continue
        tn = norm([tv[(k - transpose) % 12] for k in range(12)])
        an = norm(audv[j])
        if tn is None or an is None:
            continue
        tot += cosine(tn, an)
        n += 1
    return (tot / n, n) if n else (0.0, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gp", required=True)
    ap.add_argument("--audio", required=True)
    ap.add_argument("--beats", required=True, help="beatmap timeseries file")
    ap.add_argument("--algo", default="nnls")
    ap.add_argument("--offset-lo", type=int, default=-16)
    ap.add_argument("--offset-hi", type=int, default=64)
    ap.add_argument("--transpose", type=int, default=None,
                    help="fix the transposition instead of searching")
    ap.add_argument("--top", type=int, default=8)
    args = ap.parse_args()

    beats = read_beatmap(args.beats)
    audv = audio_chroma(args.audio, args.beats, args.algo)
    tracks = gpdump.read_tracks(args.gp)
    info = gpdump.read_info(args.gp)
    nmeas = int(info.get("measures", "0"))

    tabv = tab_pcv(args.gp, tracks, nmeas * 4 + 8)

    print("tab      : %s -- %s (%s), %d measures, %s" %
          (info.get("artist", "?"), info.get("title", "?"),
           info.get("format", "?"), nmeas, info.get("tempo", "?")))
    print("beatmap  : %d beats, %.1f s span" %
          (len(beats), beats[-1] - beats[0] if len(beats) > 1 else 0))
    print("chroma   : %d beat intervals (%s)" % (len(audv), args.algo))
    print()

    transposes = [args.transpose] if args.transpose is not None else range(-6, 6)
    results = []
    for tr in transposes:
        for off in range(args.offset_lo, args.offset_hi + 1):
            s, n = score_alignment(tabv, audv, off, tr)
            if n > 40:
                results.append((s, off, tr, n))
    results.sort(reverse=True)

    print("best alignments (score, beat offset, transpose, overlap):")
    for s, off, tr, n in results[:args.top]:
        print("  %.4f   offset=%+d  transpose=%+d semitones  n=%d" % (s, off, tr, n))

    if not results:
        print("no alignment found")
        return 1

    best_s, best_off, best_tr, _ = results[0]
    # Peak sharpness: how much better the winner is than the best alternative
    # that is neither adjacent in offset nor the same transposition.
    rival = max((s for s, off, tr, n in results
                 if abs(off - best_off) > 2 or tr != best_tr), default=0.0)
    print()
    print("chosen   : offset=%+d transpose=%+d  score=%.4f  runner-up=%.4f  margin=%.4f"
          % (best_off, best_tr, best_s, rival, best_s - rival))
    if best_s < 0.55:
        print("WARNING  : low absolute score -- tab may not match this recording")
    if best_s - rival < 0.02:
        print("WARNING  : flat score surface -- alignment is not well determined")
    return 0


if __name__ == "__main__":
    sys.exit(main())
