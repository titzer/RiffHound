#!/usr/bin/env python3
"""Ensemble of external chord recognisers, with a confidence column.

Usage: chords_ensemble.py AUDIO_FILE
       BM_CHORD_CMD="python3 scripts/chords_ensemble.py" ./bmbench ... --set chord_external=1

Runs every recogniser it can find (madmom in external/venv, BTC in
external/venv-btc -- each already caches per track) and merges them on a
10 ms grid.  Where the models agree on root and quality the label is
certain; where they disagree the highest-weighted model's label wins with
its weight as the confidence, so the decoder's emission bonus is scaled
down rather than trusted outright.  Output: start<TAB>end<TAB>label<TAB>conf.

Models and weights live in MODELS below; a new recogniser is one more entry
whose script prints start<TAB>end<TAB>label lines.  Any that fails to run is
skipped, so one missing environment does not silence the rest."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXT = os.path.join(HERE, "..", "external")
MODELS = [
    # (name, python, script, weight)
    ("madmom", os.path.join(EXT, "venv", "bin", "python"), os.path.join(HERE, "chords_madmom.py"), 0.6),
    ("btc",    os.path.join(EXT, "venv-btc", "bin", "python"), os.path.join(HERE, "chords_btc.py"), 0.4),
]
ROOTS = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
STEP = 0.01

def triad(label):
    """(root, minor) a label reduces to for voting, or None."""
    if not label or label[0] not in ROOTS: return None
    r = ROOTS[label[0]]; q = label[1:]
    if q.startswith("#"): r = (r + 1) % 12; q = q[1:]
    elif q.startswith("b"): r = (r + 11) % 12; q = q[1:]
    return (r, q.startswith("m") and not q.startswith("maj"))

def run(python, script, audio):
    if not os.path.exists(python): return None
    p = subprocess.run([python, script, audio], capture_output=True, text=True)
    if p.returncode != 0: return None
    out = []
    for line in p.stdout.splitlines():
        f = line.split("\t")
        if len(f) < 3: continue
        try: out.append((float(f[0]), float(f[1]), f[2]))
        except ValueError: pass
    return out

def main():
    if len(sys.argv) != 2: sys.exit(__doc__)
    audio = sys.argv[1]
    runs = []
    for name, python, script, w in MODELS:
        segs = run(python, script, audio)
        if segs: runs.append((name, w, segs))
    if not runs: return 1
    runs.sort(key=lambda r: -r[1])
    end = max(s[1] for _, _, segs in runs for s in segs)
    n = int(end / STEP) + 1
    # label per grid cell per model
    grids = []
    for _, w, segs in runs:
        g = [None] * n
        for t0, t1, lab in segs:
            for i in range(int(t0 / STEP), min(n, int(t1 / STEP) + 1)): g[i] = lab
        grids.append((w, g))
    total_w = sum(w for w, _ in grids)
    cur = None; cur_t0 = 0.0; out = []
    def flush(i):
        if cur and cur[0]: out.append((cur_t0, i * STEP, cur[0], cur[1]))
    for i in range(n):
        votes = {}
        for w, g in grids:
            lab = g[i]
            if not lab: continue
            key = triad(lab)
            if key is None: continue
            v = votes.setdefault(key, [0.0, lab])
            v[0] += w
        if votes:
            key, (score, lab) = max(votes.items(), key=lambda kv: kv[1][0])
            cell = (lab, round(score / total_w, 3))
        else:
            cell = (None, 0.0)
        if cell != cur:
            flush(i); cur = cell; cur_t0 = i * STEP
    flush(n)
    for t0, t1, lab, conf in out:
        if t1 - t0 >= 0.05: sys.stdout.write("%.3f\t%.3f\t%s\t%.3f\n" % (t0, t1, lab, conf))
    return 0

if __name__ == "__main__":
    sys.exit(main())
