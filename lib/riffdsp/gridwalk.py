#!/usr/bin/env python3
"""Fit a beat grid window-by-window, following tempo drift, with honest gaps.

`riffdsp grid` fits ONE tempo to a whole track.  That is right for a track cut
to a click and wrong for anything that breathes -- Stairway to Heaven runs from
about 72 BPM at the start to over 100 by the end, and a single global fit lands
on a compromise tempo that is wrong everywhere.

This walks the track in overlapping windows, seeding each window's tempo search
from the previous window's answer so the grid can drift, and **refuses to emit
beats for windows where the fit is not well supported**.  A beatmap with honest
gaps is far more useful than one that silently drifts: the gaps tell the user
exactly which 20 seconds still need a human.
"""

import argparse
import os
import re
import subprocess
import sys

RIFFDSP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "riffdsp")

_BPM_RE = re.compile(r"grid fit ([\d.]+) BPM")
_SUP_RE = re.compile(r"(\d+)/(\d+) had onset support")


def fit_window(audio, t0, t1, lo, hi, support_exp="0.5"):
    cmd = [RIFFDSP, "grid", "--start", str(t0), "--end", str(t1),
           "--min-bpm", str(lo), "--max-bpm", str(hi),
           "--support-exp", support_exp, audio]
    r = subprocess.run(cmd, capture_output=True, text=True)
    beats = [float(l.split()[0]) for l in r.stdout.splitlines()
             if not l.startswith("#") and len(l.split()) >= 3 and l.split()[2] == "B"]
    bpm = None
    m = _BPM_RE.search(r.stderr)
    if m:
        bpm = float(m.group(1))
    sup = 0.0
    m = _SUP_RE.search(r.stderr)
    if m and int(m.group(2)):
        sup = int(m.group(1)) / int(m.group(2))
    return beats, bpm, sup


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("--window", type=float, default=20.0)
    ap.add_argument("--hop", type=float, default=16.0)
    ap.add_argument("--min-bpm", type=float, default=60)
    ap.add_argument("--max-bpm", type=float, default=200)
    ap.add_argument("--drift", type=float, default=0.10,
                    help="how far tempo may move between adjacent windows")
    ap.add_argument("--min-support", type=float, default=0.55,
                    help="reject a window whose beats lack onset support")
    ap.add_argument("--duration", type=float, default=None)
    ap.add_argument("--seed-bpm", type=float, default=None)
    args = ap.parse_args()

    if args.duration is None:
        r = subprocess.run([RIFFDSP, "info", args.audio],
                           capture_output=True, text=True)
        dur = None
        for line in r.stdout.splitlines():
            if line.startswith("duration"):
                dur = float(line.split()[1])
        if dur is None:
            print("cannot determine duration", file=sys.stderr)
            return 1
        args.duration = dur

    prev_bpm = args.seed_bpm
    accepted = []      # (t0, t1, bpm, support, beats)
    rejected = []
    t = 0.0
    while t < args.duration - 2.0:
        t1 = min(t + args.window, args.duration)
        if prev_bpm:
            lo = max(args.min_bpm, prev_bpm * (1 - args.drift))
            hi = min(args.max_bpm, prev_bpm * (1 + args.drift))
        else:
            lo, hi = args.min_bpm, args.max_bpm
        beats, bpm, sup = fit_window(args.audio, t, t1, lo, hi)
        if bpm and sup >= args.min_support and len(beats) >= 4:
            accepted.append((t, t1, bpm, sup, beats))
            prev_bpm = bpm
        else:
            rejected.append((t, t1, bpm, sup))
            # Do not carry a bad tempo forward into the next window.
            if sup < 0.35:
                prev_bpm = None
        t += args.hop

    # Stitch: keep beats from the first window that covers each time, so the
    # overlap between windows does not produce doubled beats.
    out = []
    for (t0, t1, bpm, sup, beats) in accepted:
        for b in beats:
            if not out or b - out[-1] > 0.25:
                out.append(b)

    print("# Beatmap  ~src=riffdsp/gridwalk")
    print("# windows accepted %d, rejected %d" % (len(accepted), len(rejected)))
    if accepted:
        print("# tempo range %.1f - %.1f BPM" %
              (min(a[2] for a in accepted), max(a[2] for a in accepted)))
    for (t0, t1, bpm, sup) in rejected:
        print("# GAP %.1f-%.1f s  bpm=%s support=%.2f  -- needs a human"
              % (t0, t1, ("%.1f" % bpm) if bpm else "?", sup))
    for b in out:
        print("%.6f\t%.6f\tB" % (b, b))

    sys.stderr.write("gridwalk: %d beats, %d/%d windows accepted (%.0f%% of track)\n"
                     % (len(out), len(accepted), len(accepted) + len(rejected),
                        100.0 * len(accepted) / max(1, len(accepted) + len(rejected))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
