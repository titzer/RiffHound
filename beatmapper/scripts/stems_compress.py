#!/usr/bin/env python3
"""Compress cached stems from .wav to constant-bit-rate .mp3.

Usage: stems_compress.py STEM_DIR [SOURCE_AUDIO]
       stems_compress.py --all [TRACK_DIR ...]

The stem cache (see stems_demucs.py) is written as 16-bit wav because that is
what Demucs produces fastest; beatmapper spawns this script in the background
once it has loaded the stems, so the encode never sits on the critical path.
Each <stem>.wav becomes <stem>.mp3 at the source track's bit rate (snapped up
to a legal MP3 rate, 320 kbps for lossless sources).  The mp3 is written to a
temporary name and renamed into place before the wav is removed, so at any
moment a complete copy of every stem exists; readers fall back from one
extension to the other.  The encoder's LAME tag records delay and padding, so
gapless decoders (miniaudio's dr_mp3, ffmpeg) keep the stems sample-aligned
with the mix.

The source track is taken from SOURCE_AUDIO, else from the directory's
`source` file (written by stems_demucs.py).  --all compresses every cache
directory, matching old directories without a `source` file to their tracks
by recomputing the cache key over the audio files under TRACK_DIRs.

Needs ffmpeg and ffprobe (looked up on PATH and in the Homebrew prefixes,
since an app launched from Finder has a minimal PATH).
"""
import fcntl
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stems_demucs import CACHE_ROOT, cache_dir  # noqa: E402

MP3_RATES = [32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
AUDIO_EXTS = (".mp3", ".m4a", ".wav", ".flac", ".ogg", ".aiff", ".aif")


def find_tool(name):
    p = shutil.which(name)
    if p:
        return p
    for d in ("/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"):
        p = os.path.join(d, name)
        if os.access(p, os.X_OK):
            return p
    return None


def source_kbps(ffprobe, audio):
    """The audio stream's bit rate in kbps (container rate as a fallback)."""
    out = subprocess.run([ffprobe, "-v", "error", "-select_streams", "a:0",
                          "-show_entries", "stream=bit_rate,codec_name:format=bit_rate",
                          "-of", "json", audio], capture_output=True, text=True)
    if out.returncode != 0:
        return None
    info = json.loads(out.stdout or "{}")
    streams = info.get("streams") or [{}]
    codec = streams[0].get("codec_name", "")
    if codec.startswith("pcm_") or codec in ("flac", "alac", "wavpack", "ape"):
        return 320
    for br in (streams[0].get("bit_rate"), info.get("format", {}).get("bit_rate")):
        try:
            if br and int(br) > 0:
                return int(br) / 1000.0
        except ValueError:
            pass
    return None


def snap_kbps(kbps):
    if kbps is None:
        return 320
    for r in MP3_RATES:
        if r >= kbps - 0.5:     # 127.9 kbps reported for a 128k file stays 128
            return r
    return 320


def compress_dir(d, source, ffmpeg, ffprobe, log=sys.stderr):
    done = os.path.join(d, "done")
    if not os.path.exists(done):
        return "incomplete"
    names = [ln.strip() for ln in open(done) if ln.strip()]
    todo = [n for n in names if os.path.exists(os.path.join(d, n + ".wav"))]
    if not todo:
        return "already"
    if not source:
        sf = os.path.join(d, "source")
        if os.path.exists(sf):
            source = open(sf).read().strip()
    if source and os.path.exists(source):
        kbps = snap_kbps(source_kbps(ffprobe, source))
    else:
        log.write(f"[compress] {d}: source unknown, using 320 kbps\n")
        kbps = 320

    lock = open(os.path.join(d, ".compress.lock"), "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return "busy"              # another compressor has this directory
    for n in todo:
        wav = os.path.join(d, n + ".wav")
        mp3 = os.path.join(d, n + ".mp3")
        tmp = os.path.join(d, n + ".mp3.tmp")
        if not os.path.exists(wav):
            continue
        r = subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-y", "-i", wav,
                            "-codec:a", "libmp3lame", "-b:a", f"{kbps}k",
                            "-f", "mp3", tmp], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(tmp) or os.path.getsize(tmp) == 0:
            log.write(f"[compress] {wav}: ffmpeg failed: {r.stderr.strip()}\n")
            if os.path.exists(tmp):
                os.unlink(tmp)
            return "failed"
        os.replace(tmp, mp3)
        os.unlink(wav)
    log.write(f"[compress] {d}: {len(todo)} stems -> {kbps} kbps mp3\n")
    return f"{kbps}k"


def track_index(roots):
    """cache directory -> track path for the audio files under roots."""
    idx = {}
    for root in roots:
        for dp, _, files in os.walk(os.path.expanduser(root)):
            for f in files:
                if f.lower().endswith(AUDIO_EXTS):
                    p = os.path.join(dp, f)
                    try:
                        idx.setdefault(cache_dir(p, "htdemucs_6s"), os.path.realpath(p))
                    except OSError:
                        pass
    return idx


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        sys.stdout.write(__doc__)
        return 0 if args else 1
    ffmpeg, ffprobe = find_tool("ffmpeg"), find_tool("ffprobe")
    if not ffmpeg or not ffprobe:
        sys.stderr.write("[compress] ffmpeg/ffprobe not found\n")
        return 1
    try:
        os.nice(10)
    except OSError:
        pass
    if args[0] == "--all":
        idx = track_index(args[1:])
        rc = 0
        for h in sorted(os.listdir(CACHE_ROOT)):
            d = os.path.join(CACHE_ROOT, h)
            if not os.path.isdir(d):
                continue
            src = idx.get(d)
            if src and not os.path.exists(os.path.join(d, "source")):
                with open(os.path.join(d, "source"), "w") as f:
                    f.write(src + "\n")
            res = compress_dir(d, src, ffmpeg, ffprobe)
            print(f"{h}\t{res}\t{src or '?'}", flush=True)
            if res == "failed":
                rc = 1
        return rc
    d = args[0]
    src = args[1] if len(args) > 1 else None
    if src and os.path.exists(src) and not os.path.exists(os.path.join(d, "source")):
        with open(os.path.join(d, "source"), "w") as f:
            f.write(os.path.realpath(src) + "\n")
    return 1 if compress_dir(d, src, ffmpeg, ffprobe) == "failed" else 0


if __name__ == "__main__":
    sys.exit(main())
