#!/usr/bin/env python3
"""Score automatic beat detection against hand-authored beatmaps.

The 50 hand-made .txt files in playalong/ and backing_tracks/ are a labelled
evaluation set -- thousands of beats placed by a human who cared about getting
them right.  This scores a detector against them so that changing an algorithm
produces a number instead of an opinion.

Metric is the standard beat-tracking F-measure: a detected beat counts as a hit
if it falls within +/- tolerance of a reference beat, one-to-one.  70 ms is the
usual tolerance in the MIR literature.

Because several reference beatmaps cover only part of their track, scoring is
restricted by default to the time spans the reference actually covers -- a
detector should not be punished for finding beats in a region the human never
annotated.
"""

import argparse
import os
import subprocess
import sys

RIFFDSP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "riffdsp")


def read_beats(path):
    ts = []
    for line in open(path, errors="replace"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        p = line.split()
        if len(p) >= 3 and p[2] == "B":
            try:
                ts.append(float(p[0]))
            except ValueError:
                pass
    ts.sort()
    return ts


def islands(ts, gap):
    """Split a beat list into contiguous runs separated by more than `gap`."""
    if not ts:
        return []
    out = [[ts[0]]]
    for t in ts[1:]:
        if t - out[-1][-1] > gap:
            out.append([])
        out[-1].append(t)
    return [g for g in out if len(g) > 1]


def f_measure(ref, est, tol=0.070):
    """One-to-one greedy matching in time order."""
    i = j = hits = 0
    used = [False] * len(est)
    for r in ref:
        best, bi = tol, -1
        for k in range(len(est)):
            if used[k]:
                continue
            d = abs(est[k] - r)
            if d <= best:
                best, bi = d, k
            elif est[k] > r + tol:
                break
        if bi >= 0:
            used[bi] = True
            hits += 1
    p = hits / len(est) if est else 0.0
    rc = hits / len(ref) if ref else 0.0
    f = 2 * p * rc / (p + rc) if (p + rc) else 0.0
    return f, p, rc, hits


def run_detector(audio, mode, lo, hi, start=None, end=None):
    cmd = [RIFFDSP, mode, "--min-bpm", str(lo), "--max-bpm", str(hi)]
    if os.environ.get("SUPPORT_EXP"): cmd += ["--support-exp", os.environ["SUPPORT_EXP"]]
    if start is not None:
        cmd += ["--start", str(start)]
    if end is not None:
        cmd += ["--end", str(end)]
    cmd.append(audio)
    out = subprocess.run(cmd, capture_output=True, text=True)
    ts = []
    for line in out.stdout.splitlines():
        if line.startswith("#"):
            continue
        p = line.split()
        if len(p) >= 3 and p[2] == "B":
            ts.append(float(p[0]))
    return ts


def find_audio(txt):
    stem = os.path.splitext(txt)[0]
    for ext in (".mp3", ".m4a", ".wav", ".flac", ".MP3"):
        if os.path.exists(stem + ext):
            return stem + ext
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", help="reference .txt beatmaps")
    ap.add_argument("--mode", default="grid", choices=["grid", "beats"])
    ap.add_argument("--min-bpm", type=float, default=60)
    ap.add_argument("--max-bpm", type=float, default=200)
    ap.add_argument("--tol", type=float, default=0.070)
    ap.add_argument("--windowed", action="store_true",
                    help="fit each reference island separately instead of "
                         "one global fit (shows the cost of tempo drift)")
    ap.add_argument("--min-beats", type=int, default=30)
    args = ap.parse_args()

    rows = []
    for txt in args.files:
        ref = read_beats(txt)
        if len(ref) < args.min_beats:
            continue
        audio = find_audio(txt)
        if not audio:
            continue
        name = os.path.basename(txt)[:-4]
        isl = islands(ref, gap=3.0)
        if not isl:
            continue

        try:
            if args.windowed:
                est = []
                for g in isl:
                    est += run_detector(audio, args.mode, args.min_bpm,
                                        args.max_bpm, g[0] - 0.5, g[-1] + 0.5)
            else:
                est = run_detector(audio, args.mode, args.min_bpm, args.max_bpm)
        except Exception as e:
            print("%-46s ERROR %s" % (name[:46], e))
            continue

        # Restrict both sides to the spans the human actually annotated.
        spans = [(g[0] - 0.1, g[-1] + 0.1) for g in isl]
        def inside(t):
            return any(a <= t <= b for a, b in spans)
        est_in = [t for t in est if inside(t)]

        f, p, r, hits = f_measure(ref, est_in, args.tol)
        cov = sum(b - a for a, b in spans)
        rows.append((f, name, len(ref), len(est_in), p, r, cov))

    rows.sort()
    print("%-44s %5s %5s %6s %6s %6s  %s" %
          ("track", "ref", "est", "F", "prec", "rec", "annotated s"))
    print("-" * 92)
    for f, name, nref, nest, p, r, cov in rows:
        print("%-44s %5d %5d %6.3f %6.3f %6.3f  %8.1f" %
              (name[:44], nref, nest, f, p, r, cov))
    if rows:
        mean = sum(x[0] for x in rows) / len(rows)
        good = sum(1 for x in rows if x[0] >= 0.80)
        print("-" * 92)
        print("%d tracks   mean F = %.3f   F>=0.80 on %d (%.0f%%)" %
              (len(rows), mean, good, 100.0 * good / len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
