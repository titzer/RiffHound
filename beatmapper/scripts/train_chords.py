#!/usr/bin/env python3
"""Train a transposition-tied chord emission model on the mapped corpus.

    ./bmbench bench/tracks-anno chroma > chroma.tsv
    scripts/train_chords.py chroma.tsv [--header src/chord_model.h] [--context] [--no-eval]

Input: one line per truth beat -- track, beat index, t0, t1, chord token,
twelve chroma values (the bench's `chroma` command).  Beats whose chord
parses as a major or minor triad train the model; a 7 counts as major, m7 as
minor, sus / power / dim / aug beats are skipped.

The model scores chord (root r, quality q) on a beat as  w_q . rot(x, r) + b_q
where rot rotates the L2-normalised chroma so the root is pitch class 0 and
x carries, with --context, the mean chroma of the neighbouring beats as a
second 12-vector.  One weight vector per quality serves every root, so all
keys and all tracks train the same 2 x (12 or 24) + 2 numbers, fitted by
softmax regression over the 24 candidates.  Reported per-beat accuracy is
leave-one-track-out, nearest-candidate with no transition model -- the
emission alone -- next to the hand triad (1, .8, .8) cosine the decoder used
before.  --header writes the weights for the decoder."""
import sys, collections
import numpy as np

ROOTS = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}

def parse(tok):
    if not tok or tok[0] not in ROOTS: return None
    r = ROOTS[tok[0]]; q = tok[1:]
    if q.startswith("#"): r = (r + 1) % 12; q = q[1:]
    elif q.startswith("b"): r = (r + 11) % 12; q = q[1:]
    if q.startswith("sus") or q == "5" or q.startswith("dim") or q.startswith("aug"): return None
    minor = q.startswith("m") and not q.startswith("maj")
    return r, minor

def load(path, context):
    per_track = collections.defaultdict(list)
    for line in open(path):
        f = line.rstrip("\n").split("\t")
        if len(f) < 17: continue
        per_track[f[0]].append((int(f[1]), f[4], np.array([float(x) for x in f[5:17]])))
    rows = []
    for t, beats in per_track.items():
        beats.sort()
        X = np.array([b[2] for b in beats])
        X = X / np.maximum(np.linalg.norm(X, axis=1, keepdims=True), 1e-9)
        if context:
            prev = np.vstack([X[:1], X[:-1]]); nxt = np.vstack([X[1:], X[-1:]])
            C = 0.5 * (prev + nxt)
            X = np.hstack([X, C])
        for i, (_, tok, _) in enumerate(beats):
            pr = parse(tok)
            if pr: rows.append((t, pr[0], pr[1], X[i]))
    return rows

def candidates(x):
    """24 x D matrix: x rotated so candidate root r sits at 0, for r in 0..11, twice (maj, min)."""
    D = x.shape[0]; blocks = D // 12
    rots = []
    for r in range(12):
        rots.append(np.concatenate([np.roll(x[b * 12:(b + 1) * 12], -r) for b in range(blocks)]))
    R = np.array(rots)
    return np.vstack([R, R])           # rows 0..11 major candidates, 12..23 minor

def scores(W, b, x):
    C = candidates(x)                  # 24 x D
    q = np.array([0] * 12 + [1] * 12)
    return np.einsum("cd,cd->c", C, W[q]) + b[q]

def fit(rows, epochs=60, lr=0.5, l2=1e-3, seed=0):
    D = rows[0][3].shape[0]
    W = np.zeros((2, D)); b = np.zeros(2)
    rng = np.random.default_rng(seed)
    idx = np.arange(len(rows))
    for ep in range(epochs):
        rng.shuffle(idx)
        gW = np.zeros_like(W); gb = np.zeros_like(b); n = 0
        for i in idx:
            _, r, minor, x = rows[i]
            C = candidates(x); q = np.array([0] * 12 + [1] * 12)
            s = np.einsum("cd,cd->c", C, W[q]) + b[q]
            p = np.exp(s - s.max()); p /= p.sum()
            y = r + (12 if minor else 0)
            p[y] -= 1.0
            for c in range(24):
                gW[q[c]] += p[c] * C[c]; gb[q[c]] += p[c]
            n += 1
            if n % 256 == 0:
                W -= lr * (gW / n + l2 * W); b -= lr * gb / n
                gW[:] = 0; gb[:] = 0; n = 0
        if n:
            W -= lr * (gW / n + l2 * W); b -= lr * gb / n
        lr *= 0.95
    return W, b

def accuracy(rows, W, b):
    ok = 0
    for _, r, minor, x in rows:
        c = int(np.argmax(scores(W, b, x)))
        if c == r + (12 if minor else 0): ok += 1
    return ok

def triad_accuracy(rows):
    tri = {0: np.array([1, 0, 0, 0, .8, 0, 0, .8, 0, 0, 0, 0.]), 1: np.array([1, 0, 0, .8, 0, 0, 0, .8, 0, 0, 0, 0.])}
    ok = 0
    for _, r, minor, x in rows:
        v = x[:12]
        best, bs = None, -2
        for q in (0, 1):
            for rr in range(12):
                s = float(np.dot(v, np.roll(tri[q], rr)) / (np.linalg.norm(v) * np.linalg.norm(tri[q]) + 1e-9))
                if s > bs: bs, best = s, rr + 12 * q
        if best == r + (12 if minor else 0): ok += 1
    return ok

def main():
    path = sys.argv[1]; header = None; context = "--context" in sys.argv
    if "--header" in sys.argv: header = sys.argv[sys.argv.index("--header") + 1]
    rows = load(path, context)
    tracks = sorted(set(r[0] for r in rows))
    tot = collections.Counter()
    for t in ([] if "--no-eval" in sys.argv else tracks):
        train = [r for r in rows if r[0] != t]; test = [r for r in rows if r[0] == t]
        W, b = fit(train)
        ok_l = accuracy(test, W, b); ok_t = triad_accuracy(test)
        tot["learned"] += ok_l; tot["triad"] += ok_t; tot["n"] += len(test)
        print("%-32s beats %4d  triad %5.1f%%  learned %5.1f%%" % (t, len(test), 100.0 * ok_t / len(test), 100.0 * ok_l / len(test)), flush=True)
    if tot["n"]:
        print("TOTAL %d beats: triad %.1f%%  learned (leave-one-track-out) %.1f%%%s" %
              (tot["n"], 100.0 * tot["triad"] / tot["n"], 100.0 * tot["learned"] / tot["n"], "  [context]" if context else ""))
    W, b = fit(rows)
    print("major w: %s  b %.3f" % (" ".join("%.2f" % x for x in W[0]), b[0]))
    print("minor w: %s  b %.3f" % (" ".join("%.2f" % x for x in W[1]), b[1]))
    if header:
        D = W.shape[1]
        with open(header, "w") as h:
            h.write("// Generated by scripts/train_chords.py from the mapped corpus (%d beats, %d tracks).\n"
                    "// Chord emission: score(root r, quality q) = w_q . rot(x, r) + b_q, where x is\n"
                    "// the L2-normalised beat chroma%s rotated so r is pitch class 0.\n#pragma once\n"
                    % (len(rows), len(tracks), " followed by the neighbouring beats' mean chroma" if context else ""))
            h.write("#define CHORD_MODEL_DIM %d\n" % D)
            h.write("#define CHORD_MODEL_CONTEXT %d\n" % (1 if context else 0))
            h.write("static const float CHORD_MODEL_W[2][%d] = {\n  { %s },\n  { %s } };\n" %
                    (D, ", ".join("%.4ff" % x for x in W[0]), ", ".join("%.4ff" % x for x in W[1])))
            h.write("static const float CHORD_MODEL_B[2] = { %.4ff, %.4ff };\n" % (b[0], b[1]))
        print("wrote", header)

if __name__ == "__main__":
    main()
