#!/usr/bin/env python3
"""Chord recognition via madmom's CNN feature + CRF decoder
(Korzeniowski & Widmer; ~83% maj/min WCSR on Isophonics).

Usage: chords_madmom.py AUDIO_FILE

Prints one line per chord to stdout:  start<TAB>end<TAB>label
Labels are beatmapper-style ("C", "Am"); no-chord spans are omitted.

Results are cached in ~/.cache/beatmapper/chords/ keyed on the audio file's
path, size and mtime, so repeated runs (the bench sweeps the same tracks
hundreds of times) cost one CNN pass per track.

Run with beatmapper/external/venv/bin/python (madmom installed from git main;
the PyPI 0.16.1 release does not build on modern numpy).
"""
import hashlib
import os
import sys


def cache_path(audio):
    st = os.stat(audio)
    key = f"{os.path.abspath(audio)}|{st.st_size}|{int(st.st_mtime)}|madmom-cnn-crf"
    h = hashlib.sha1(key.encode()).hexdigest()[:24]
    d = os.path.expanduser("~/.cache/beatmapper/chords")
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, h + ".tsv")


def to_beatmapper_label(label):
    # madmom: "C:maj", "A:min", "F#:maj", "N"
    if label == "N":
        return None
    root, _, qual = label.partition(":")
    return root + ("m" if qual == "min" else "")


def recognize(audio):
    from madmom.features.chords import (CNNChordFeatureProcessor,
                                        CRFChordRecognitionProcessor)
    feats = CNNChordFeatureProcessor()(audio)
    return CRFChordRecognitionProcessor()(feats)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    audio = sys.argv[1]
    if not os.path.exists(audio):
        sys.stderr.write(f"no such file: {audio}\n")
        return 1
    cp = cache_path(audio)
    if os.path.exists(cp):
        sys.stdout.write(open(cp).read())
        return 0
    lines = []
    for start, end, label in recognize(audio):
        bm = to_beatmapper_label(label)
        if bm is None:
            continue
        lines.append(f"{float(start):.3f}\t{float(end):.3f}\t{bm}\n")
    out = "".join(lines)
    tmp = cp + ".tmp"
    with open(tmp, "w") as f:
        f.write(out)
    os.replace(tmp, cp)
    sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
