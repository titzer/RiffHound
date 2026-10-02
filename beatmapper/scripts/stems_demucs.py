#!/usr/bin/env python3
"""Stem separation via Demucs (Meta's Hybrid Transformer Demucs).

Usage: stems_demucs.py AUDIO_FILE [--model NAME] [--device auto|cpu|mps|cuda]
                                  [--check]

Default model htdemucs_6s splits a mix into six stems:
    vocals  drums  bass  guitar  piano  other
(htdemucs_ft / htdemucs give four: vocals drums bass other, slightly cleaner
on those four at twice the cost.)

Prints to stdout, one per line, tab separated:
    progress<TAB>0.42            while separating (fraction done)
    status<TAB>text              phase changes (loading model, writing ...)
    stem<TAB>name<TAB>/path.wav  one per stem, when finished
Exit codes: 0 done, 3 (--check only) not cached, 1 failure.

--check answers from the cache without importing torch (fast): the stems are
written to ~/.cache/beatmapper/stems/<hash>/<stem>.wav keyed on the audio
file's path, size, mtime and the model, so each track is separated once.

Run with beatmapper/external/venv-stems/bin/python (scripts/setup-stems.sh).
"""
import argparse
import hashlib
import os
import sys
import time

STEM_ORDER = ["vocals", "drums", "bass", "guitar", "piano", "other"]


def cache_dir(audio, model):
    st = os.stat(audio)
    # realpath: the same track reached through a symlink shares the cache
    key = f"{os.path.realpath(audio)}|{st.st_size}|{int(st.st_mtime)}|demucs-{model}"
    h = hashlib.sha1(key.encode()).hexdigest()[:24]
    d = os.path.expanduser("~/.cache/beatmapper/stems")
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, h)


def emit(*fields):
    sys.stdout.write("\t".join(str(f) for f in fields) + "\n")
    sys.stdout.flush()


def cached_stems(d):
    """List of (name, path) if the cache directory is complete, else None."""
    done = os.path.join(d, "done")
    if not os.path.exists(done):
        return None
    names = [ln.strip() for ln in open(done) if ln.strip()]
    out = []
    for n in names:
        p = os.path.join(d, n + ".wav")
        if not os.path.exists(p):
            return None
        out.append((n, p))
    return out


def order(names):
    return sorted(names, key=lambda n: (STEM_ORDER.index(n) if n in STEM_ORDER else 99, n))


def pick_device(want):
    import torch
    if want != "auto":
        return want
    if torch.cuda.is_available():
        return "cuda"
    if getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def separate(audio, model_name, device, out_dir):
    # Ops Demucs needs that MPS lacks fall back to the CPU instead of failing.
    os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")
    import torch
    from demucs.api import Separator, save_audio

    emit("status", f"loading {model_name}")
    device = pick_device(device)

    def make_cb():
        def cb(info):
            if info.get("state") != "end":
                return
            models = max(1, info.get("models", 1))
            shifts = max(1, info.get("shifts", 1))
            length = max(1, info.get("audio_length", 1))
            done = ((info.get("model_idx_in_bag", 0) * shifts + info.get("shift_idx", 0)) * length
                    + info.get("segment_offset", 0) + info.get("segment_length", 0))
            emit("progress", f"{min(1.0, done / (models * shifts * length)):.3f}")
        return cb

    def run(dev):
        sep = Separator(model=model_name, device=dev, progress=False, callback=make_cb())
        emit("status", f"separating on {dev}")
        t0 = time.time()
        origin, parts = sep.separate_audio_file(audio)
        sys.stderr.write(f"separated {audio} on {dev} in {time.time() - t0:.1f}s\n")
        return sep, parts

    try:
        sep, parts = run(device)
    except Exception as e:  # e.g. an MPS op without a fallback
        if device == "cpu":
            raise
        sys.stderr.write(f"{device} failed ({e!r}); retrying on cpu\n")
        sep, parts = run("cpu")

    emit("status", "writing stems")
    os.makedirs(out_dir, exist_ok=True)
    names = order(list(parts.keys()))
    for n in names:
        save_audio(parts[n], os.path.join(out_dir, n + ".wav"), samplerate=sep.samplerate,
                   clip="clamp", bits_per_sample=16)
    with open(os.path.join(out_dir, "done"), "w") as f:
        f.write("\n".join(names) + "\n")
    return [(n, os.path.join(out_dir, n + ".wav")) for n in names]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("audio")
    ap.add_argument("--model", default="htdemucs_6s")
    ap.add_argument("--device", default="auto")
    ap.add_argument("--check", action="store_true", help="only answer from the cache")
    a = ap.parse_args()
    if not os.path.exists(a.audio):
        sys.stderr.write(f"no such file: {a.audio}\n")
        return 1
    d = cache_dir(a.audio, a.model)
    stems = cached_stems(d)
    if stems is None:
        if a.check:
            return 3
        try:
            stems = separate(a.audio, a.model, a.device, d)
        except Exception as e:
            sys.stderr.write(f"separation failed: {e!r}\n")
            emit("status", f"failed: {e}")
            return 1
    for n, p in stems:
        emit("stem", n, p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
