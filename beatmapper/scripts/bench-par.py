#!/usr/bin/env python3
"""bench-par.py BINARY TRACKDIR ONLY [extra bmbench args...]

One bmbench process per track, in parallel; the TSV rows are concatenated
(header once) on stdout, in track order.  Anything else a track printed
(--list lines, warnings) follows on stderr, prefixed with the track name.
BENCH_JOBS sets the parallelism (default: the machine's cores)."""
import os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

EXTS = (".mp3", ".m4a", ".wav", ".flac", ".ogg")

def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    binary, trackdir, only, extra = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    tracks = []
    for name in sorted(os.listdir(trackdir)):
        if not name.endswith(".txt"): continue
        stem = os.path.join(trackdir, name[:-4])
        for ext in EXTS:
            if os.path.exists(stem + ext):
                tracks.append(stem + ext); break
    jobs = int(os.environ.get("BENCH_JOBS", os.cpu_count() or 4))

    def run(audio):
        cmd = [binary, audio, "suite", "--only", only, "--tsv"] + extra
        p = subprocess.run(cmd, capture_output=True, text=True)
        return audio, p.returncode, p.stdout, p.stderr

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        results = list(ex.map(run, tracks))

    header = subprocess.run([binary, tracks[0], "suite", "--only", "NONE", "--tsv", "--header"],
                            capture_output=True, text=True).stdout.splitlines()[:1]
    out = sys.stdout
    if header: out.write(header[0] + "\n")
    for audio, rc, so, se in results:
        base = os.path.basename(audio)
        for line in so.splitlines():
            if line.count("\t") >= 10: out.write(line + "\n")
            else: sys.stderr.write(base + "\t" + line + "\n")
        for line in se.splitlines():
            if line.startswith("[beatmap]") or not line.strip(): continue
            sys.stderr.write(base + "\t" + line + "\n")
        if rc != 0: sys.stderr.write(base + "\tFAILED rc=%d\n" % rc)

if __name__ == "__main__":
    main()
