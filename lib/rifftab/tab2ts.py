#!/usr/bin/env python3
"""Turn an aligned Guitar Pro tab into RiffHound timeseries layers.

Given a tab, an audio file, a beatmap, and the (offset, transpose) found by
align.py, this emits:

  * chord:   events, inferred from the tab's exact pitches (not from chroma --
             the tab knows what the notes are, so the harmony is exact)
  * section: events, from GP rehearsal markers when the file has them
  * pattern:/play: events, from repetition analysis of the chord sequence
  * note:    events for one track, as a sidecar

See beatmapper/format-spec-chords.md for the format.
"""

import argparse
import math
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpdump
from align import read_beatmap

PC = gpdump.PC_NAMES

# Section keywords from format-spec.md, and the marker text that maps to each.
SECTION_KEYWORDS = ["intro", "verse", "pre-chorus", "chorus", "post-chorus",
                    "bridge", "breakdown", "instrumental", "solo", "interlude",
                    "outro", "refrain"]


def normalize_marker(text):
    """Map a GP rehearsal marker to (section_kind, label) or None."""
    t = text.strip().strip("[]").strip().lower()
    label = ""
    # trailing number becomes the label: "Verse 2" -> ("verse", "2")
    parts = t.split()
    if parts and parts[-1].isdigit():
        label = parts[-1]
        t = " ".join(parts[:-1])
    t = t.replace("_", "-").replace(" ", "-")
    aliases = {
        "pre-chorus": "pre-chorus", "prechorus": "pre-chorus",
        "guitar-solo": "solo", "lead": "solo",
        "interlude": "interlude", "break": "breakdown",
        "ending": "outro", "end": "outro", "coda": "outro",
        "intro": "intro", "riff": "instrumental",
    }
    if t in aliases:
        t = aliases[t]
    if t in SECTION_KEYWORDS:
        return (t, label)
    for k in SECTION_KEYWORDS:
        if t.startswith(k):
            return (k, label)
    return None


# ---------------------------------------------------------------------------
# Chord inference from exact pitches
# ---------------------------------------------------------------------------

QUALITIES = [
    # (name, required intervals, optional-but-scoring intervals)
    ("maj7", {0, 4, 7, 11}),
    ("m7",   {0, 3, 7, 10}),
    ("7",    {0, 4, 7, 10}),
    ("dim",  {0, 3, 6}),
    ("m",    {0, 3, 7}),
    ("",     {0, 4, 7}),
    ("sus4", {0, 5, 7}),
    ("sus2", {0, 2, 7}),
    ("5",    {0, 7}),
]


def infer_chord(weights, bass_pc, allow_slash=True):
    """Pick the chord symbol best explaining a weighted pitch-class histogram.

    weights: 12 floats.  bass_pc: pitch class of the lowest sounding note.
    Exact pitches make this far more reliable than the chroma-based path: the
    only real ambiguity left is which note is the root.
    """
    total = sum(weights)
    if total <= 0:
        return None
    present = {i for i in range(12) if weights[i] > 0.08 * total}
    if not present:
        return None

    best = None
    for root in range(12):
        iv = {(p - root) % 12 for p in present}
        for name, req in QUALITIES:
            if not req <= iv:
                continue
            # Reward explaining more of the weight, penalise leftover notes and
            # prefer the bass as root.  Power chords score lower than triads so
            # a real third wins when one is present.
            explained = sum(weights[(root + k) % 12] for k in req)
            extra = len(iv - req)
            score = explained / total - 0.10 * extra + len(req) * 0.02
            if root == bass_pc:
                score += 0.15
            if best is None or score > best[0]:
                best = (score, root, name)
    if best is None:
        return None
    _, root, name = best
    sym = PC[root] + name
    if allow_slash and bass_pc is not None and bass_pc != root and bass_pc in present:
        sym += "/" + PC[bass_pc]
    return sym


def beat_harmony(gpfile, tracks, nbeats, transpose):
    """Per-gp-beat (weights[12], bass_pc) summed over all melodic tracks."""
    weights = [[0.0] * 12 for _ in range(nbeats)]
    lowest = [None] * nbeats
    for t in tracks:
        if t.percussion or not t.tuning:
            continue
        beats, _ = gpdump.read_beats(gpfile, t.number)
        onsets = [b for b in beats if not b.rest and b.notes]
        for i, b in enumerate(onsets):
            end = onsets[i + 1].gp_beat if i + 1 < len(onsets) else b.gp_beat + 1.0
            for (s, f, flags) in b.notes:
                if "x" in flags:
                    continue
                p = t.pitch(s, f)
                if p is None:
                    continue
                p += transpose
                lo, hi = b.gp_beat, min(end, b.gp_beat + 8.0)
                k = int(math.floor(lo))
                while k < hi and k < nbeats:
                    if k >= 0:
                        ov = min(hi, k + 1) - max(lo, k)
                        if ov > 0:
                            weights[k][p % 12] += ov
                            if lowest[k] is None or p < lowest[k]:
                                lowest[k] = p
                    k += 1
    return weights, lowest


# ---------------------------------------------------------------------------
# Re-rolling: find repeated chord loops
# ---------------------------------------------------------------------------

def measure_chords(chords_per_beat, beats_per_measure, nmeasures):
    """Collapse per-beat chords into a per-measure tuple signature."""
    out = []
    for m in range(nmeasures):
        seg = chords_per_beat[m * beats_per_measure:(m + 1) * beats_per_measure]
        out.append(tuple(seg))
    return out


def find_patterns(mchords, min_len=2, max_len=16, min_repeats=2):
    """Find repeating measure-level loops.

    Returns a list of (name, length, [start_measure...], variations) where the
    first start is the canonical definition.  Longer loops are preferred, and a
    measure already covered by an accepted loop is not reused, so the result is
    a small set of distinct loops rather than every substring that repeats.
    """
    n = len(mchords)
    taken = [False] * n
    results = []

    for L in range(max_len, min_len - 1, -1):
        # Group candidate windows by their exact chord signature.
        buckets = defaultdict(list)
        for i in range(0, n - L + 1):
            if any(taken[i:i + L]):
                continue
            sig = tuple(mchords[i:i + L])
            if all(c == () or all(x is None for x in c) for c in sig):
                continue      # all-silent window
            buckets[sig].append(i)

        for sig, starts in buckets.items():
            # keep only non-overlapping occurrences
            keep = []
            last_end = -1
            for s in starts:
                if s >= last_end and not any(taken[s:s + L]):
                    keep.append(s)
                    last_end = s + L
            if len(keep) < min_repeats:
                continue
            for s in keep:
                for k in range(s, s + L):
                    taken[k] = True
            results.append({"len": L, "starts": keep, "sig": sig})

    results.sort(key=lambda r: r["starts"][0])
    return results


def find_variations(mchords, patterns, tol_measures=1):
    """Attach near-miss occurrences to an existing pattern as variations.

    A window that differs from a pattern in at most tol_measures measures is
    recorded as an instance with a ~var= delta rather than as a new pattern --
    this is what turns 'four slightly different verses' into 'one loop played
    four times, the last with a different ending'.
    """
    n = len(mchords)
    covered = set()
    for p in patterns:
        for s in p["starts"]:
            covered.update(range(s, s + p["len"]))

    for p in patterns:
        L = p["len"]
        sig = p["sig"]
        extra = []
        for i in range(0, n - L + 1):
            if any(k in covered for k in range(i, i + L)):
                continue
            window = tuple(mchords[i:i + L])
            diffs = [k for k in range(L) if window[k] != sig[k]]
            if 0 < len(diffs) <= tol_measures:
                extra.append((i, diffs, window))
                covered.update(range(i, i + L))
        p["variants"] = extra
    return patterns


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gp", required=True)
    ap.add_argument("--beats", required=True, help="beatmap timeseries file")
    ap.add_argument("--offset", type=int, required=True,
                    help="beatmap index of gp_beat 0 (from align.py)")
    ap.add_argument("--transpose", type=int, default=0)
    ap.add_argument("--note-track", type=int, default=1,
                    help="GP track to emit note: events for")
    ap.add_argument("--notes-out", default=None,
                    help="write note: events here instead of stdout")
    ap.add_argument("--src", default="tab/gp")
    ap.add_argument("--no-patterns", action="store_true")
    ap.add_argument("--harmonic-rate", type=int, default=4,
                    help="beats per harmony window (4 = one chord per measure). "
                         "Riff-based songs need a coarse rate; per-beat labelling "
                         "turns a single-note riff into chord salad.")
    ap.add_argument("--no-slash", action="store_true",
                    help="never emit /bass; the pedal tones in riff-based songs "
                         "produce mostly spurious slash chords")
    args = ap.parse_args()

    beats = read_beatmap(args.beats)
    nbm = len(beats)
    tracks = gpdump.read_tracks(args.gp)
    info = gpdump.read_info(args.gp)
    markers = gpdump.read_markers(args.gp)
    nmeas = int(info.get("measures", "0"))
    _, measures = gpdump.read_beats(args.gp, tracks[0].number)

    ngp = nmeas * 4 + 8
    weights, lowest = beat_harmony(args.gp, tracks, ngp, args.transpose)

    def bm_time(idx):
        """Beatmap time for beat index idx (0-based), clamped."""
        if idx < 0:
            return beats[0]
        if idx >= nbm:
            return beats[-1]
        return beats[idx]

    # --- chords per harmony window ----------------------------------------
    # Harmony has its own rhythm, usually one chord per measure or half-measure.
    # Summing the pitch weights over a window and labelling that is both more
    # accurate and far more readable than labelling every beat.
    R = max(1, args.harmonic_rate)
    chord_at = [None] * nbm
    for w0 in range(0, ngp, R):
        acc = [0.0] * 12
        low = None
        for g in range(w0, min(w0 + R, ngp)):
            for k in range(12):
                acc[k] += weights[g][k]
            if lowest[g] is not None and (low is None or lowest[g] < low):
                low = lowest[g]
        bass = (low % 12) if low is not None else None
        sym = infer_chord(acc, bass, allow_slash=not args.no_slash)
        for g in range(w0, min(w0 + R, ngp)):
            j = g + args.offset
            if 0 <= j < nbm:
                chord_at[j] = sym

    print("# Chords and structure derived from tab")
    print("#   tab   : %s -- %s (%s)" % (info.get("artist", "?"),
                                         info.get("title", "?"),
                                         os.path.basename(args.gp)))
    print("#   align : gp_beat 0 -> beatmap index %+d, transpose %+d semitones"
          % (args.offset, args.transpose))

    # --- sections from GP markers -----------------------------------------
    if markers:
        print("# Sections")
        mk = sorted(markers.items())
        for i, (mnum, text) in enumerate(mk):
            kind = normalize_marker(text)
            if not kind:
                continue
            gstart = measures.get(mnum, {}).get("gp_beat")
            if gstart is None:
                continue
            if i + 1 < len(mk):
                gend = measures.get(mk[i + 1][0], {}).get("gp_beat", gstart + 16)
            else:
                gend = ngp - 8
            j0 = int(gstart) + args.offset
            j1 = int(gend) + args.offset
            if j1 <= j0:
                continue
            name = kind[0] + (": " + kind[1] if kind[1] else "")
            print("%.6f\t%.6f\t%s @4/4\t# ~beats=B%d-B%d ~src=%s/marker"
                  % (bm_time(j0), bm_time(j1), name, j0 + 1, j1 + 1, args.src))

    # --- chord events ------------------------------------------------------
    print("# Chords")
    i = 0
    nchord = 0
    while i < nbm:
        c = chord_at[i]
        j = i
        while j + 1 < nbm and chord_at[j + 1] == c:
            j += 1
        if c is not None:
            print("%.6f\t%.6f\tchord: %s\t# ~beats=B%d-B%d ~src=%s"
                  % (bm_time(i), bm_time(j + 1), c, i + 1, j + 2, args.src))
            nchord += 1
        i = j + 1

    # --- patterns ----------------------------------------------------------
    if not args.no_patterns:
        # Measure grid anchored on the tab's barlines, expressed in beatmap beats.
        bpm_measure = 4
        first = args.offset % bpm_measure
        nm = (nbm - first) // bpm_measure
        seq = []
        for m in range(nm):
            k = first + m * bpm_measure
            seq.append(tuple(chord_at[k:k + bpm_measure]))
        pats = find_patterns(seq)
        pats = find_variations(seq, pats)

        print("# Patterns (re-rolled loops; see format-spec-chords.md)")
        used = 0
        for p in pats:
            if len(p["starts"]) + len(p.get("variants", [])) < 2:
                continue
            used += 1
            name = "loop%d" % used
            L = p["len"]
            def mrange(m):
                a = first + m * bpm_measure
                b = a + L * bpm_measure
                return a, min(b, nbm - 1)
            a, b = mrange(p["starts"][0])
            print("%.6f\t%.6f\tpattern: %s @4/4\t# ~measures=%d ~plays=%d ~beats=B%d-B%d"
                  % (bm_time(a), bm_time(b), name, L,
                     len(p["starts"]) + len(p.get("variants", [])), a + 1, b + 1))
            for s in p["starts"][1:]:
                a, b = mrange(s)
                print("%.6f\t%.6f\tplay: %s\t# ~beats=B%d-B%d" % (bm_time(a), bm_time(b), name, a + 1, b + 1))
            for (s, diffs, window) in p.get("variants", []):
                a, b = mrange(s)
                vd = ";".join("M%d:%s" % (d + 1, "/".join(str(x) for x in window[d] if x))
                              for d in diffs)
                print("%.6f\t%.6f\tplay: %s\t# ~beats=B%d-B%d ~var=%s"
                      % (bm_time(a), bm_time(b), name, a + 1, b + 1, vd[:60]))

    # --- note sidecar ------------------------------------------------------
    if args.notes_out:
        tr = next((t for t in tracks if t.number == args.note_track), None)
        if tr and not tr.percussion:
            gbeats, _ = gpdump.read_beats(args.gp, tr.number)
            onsets = [b for b in gbeats if not b.rest and b.notes]
            with open(args.notes_out, "w") as f:
                f.write("# Notes from %s track %d (%s)\n"
                        % (os.path.basename(args.gp), tr.number, tr.name))
                for i2, b in enumerate(onsets):
                    end = onsets[i2 + 1].gp_beat if i2 + 1 < len(onsets) else b.gp_beat + 1.0
                    t0 = gp_to_time(b.gp_beat, args.offset, beats)
                    t1 = gp_to_time(end, args.offset, beats)
                    for (s, fr, flags) in b.notes:
                        if "x" in flags:
                            continue
                        p = tr.pitch(s, fr)
                        if p is None:
                            continue
                        f.write("%.6f\t%.6f\tnote: gtr%d p=%d s=%d f=%d\n"
                                % (t0, t1, tr.number, p + args.transpose, s, fr))

    sys.stderr.write("tab2ts: %d chord events over %d beats\n" % (nchord, nbm))
    return 0


def gp_to_time(gp_beat, offset, beats):
    """Interpolate a fractional gp_beat into beatmap seconds."""
    x = gp_beat + offset
    i = int(math.floor(x))
    frac = x - i
    if i < 0:
        return beats[0]
    if i + 1 >= len(beats):
        return beats[-1]
    return beats[i] + frac * (beats[i + 1] - beats[i])


if __name__ == "__main__":
    sys.exit(main())
