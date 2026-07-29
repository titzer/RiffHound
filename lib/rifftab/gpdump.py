"""Parse the output of lib/gpx/gpxdumper into Python structures.

The Virgil parser in lib/gpx already reads GP3/GP4/GP5 correctly, so this module
does not re-implement the binary format; it shells out to gpxdumper and reads
its text output.  That keeps exactly one Guitar Pro parser in the project.

Guitar Pro stores music in *musical* time (ticks, 960 per quarter note, with the
first beat of the first measure at tick 960 -- the TuxGuitar convention).  A
RiffHound beatmap stores *physical* time.  Converting between them is the whole
point of the beatmap, so the conversion here is deliberately simple:

    gp_beat = (tick - 960) / 960.0

where gp_beat 0.0 is measure 1, beat 1.
"""

import re
import subprocess
import os

GPXDUMPER = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "..", "gpx", "gpxdumper")

TICKS_PER_QUARTER = 960
TICK_ORIGIN = 960          # tick of measure 1, beat 1

PC_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


class Track:
    def __init__(self, number, name, nstrings, tuning, percussion):
        self.number = number
        self.name = name
        self.nstrings = nstrings
        self.tuning = tuning          # MIDI value per string, highest string first
        self.percussion = percussion

    def pitch(self, string, fret):
        """MIDI pitch for a (1-indexed string, fret) pair, or None if unplayable.

        GP numbers strings 1..N from the highest-pitched down, and uses fret 255
        as a sentinel for a dead/muted string with no definite pitch."""
        if self.percussion or fret >= 100:
            return None
        if string < 1 or string > len(self.tuning):
            return None
        return self.tuning[string - 1] + fret

    def __repr__(self):
        return "Track(%d, %r, tuning=%r)" % (self.number, self.name, self.tuning)


class Beat:
    """One rhythmic event in one voice of one track."""
    __slots__ = ("measure", "voice", "tick", "gp_beat", "notes", "rest",
                 "num", "den", "tempo", "marker")

    def __init__(self):
        self.notes = []      # list of (string, fret, flags:set)
        self.rest = False
        self.marker = None


def _run(gpfile, *args):
    cmd = [GPXDUMPER] + list(args) + [gpfile]
    out = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        raise RuntimeError("gpxdumper failed: %s" % out.stderr[:400])
    return out.stdout


_TRACK_RE = re.compile(
    r"^\s*(\d+):\s+(.*?)\s{2,}(\d+) str\s+ch \d+/\d+(?:\s+\[perc\])?"
    r"(?:\s+tuning\s+([\d ]+))?\s*$")


def read_tracks(gpfile):
    out = _run(gpfile, "--list-tracks")
    tracks = []
    for line in out.splitlines():
        m = _TRACK_RE.match(line)
        if not m:
            continue
        num = int(m.group(1))
        name = m.group(2).strip()
        nstr = int(m.group(3))
        perc = "[perc]" in line
        tuning = [int(x) for x in m.group(4).split()] if m.group(4) else []
        tracks.append(Track(num, name, nstr, tuning, perc))
    return tracks


def read_info(gpfile):
    out = _run(gpfile, "--info")
    info = {}
    for line in out.splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            info[k.strip().lower()] = v.strip()
    return info


def read_markers(gpfile):
    """Return {measure_number: marker_text}.  Many GP files carry a full
    section map here -- intro / verse / chorus / solo -- for free."""
    out = _run(gpfile, "--markers")
    markers = {}
    for line in out.splitlines():
        m = re.match(r"^\s*M\s*(\d+):\s*(.*?)\s*$", line)
        if m:
            markers[int(m.group(1))] = m.group(2)
    return markers


_M_RE = re.compile(r"^\s*M\s*(\d+)\s*\[(\d+)/(\d+)\s+q=(\d+)\](.*)$")
_B_RE = re.compile(r"^\s*V(\d+)\s+B\s*(\d+)\s*\[\s*(\d+)\]:\s*(\S+)\s*(.*)$")
_NOTE_RE = re.compile(r"s(\d+)f(\d+)((?:\([a-z]+\))*)")


def read_beats(gpfile, track_number, voice=None):
    """Return (beats, measures).

    beats:    list of Beat, in tick order, for the requested track
    measures: {measure_number: (num, den, tempo, first_gp_beat)}
    """
    args = ["--beats", "-t", str(track_number)]
    if voice is not None:
        args += ["-v", str(voice)]
    out = _run(gpfile, *args)

    beats = []
    measures = {}
    cur_m = None
    cur_num = cur_den = cur_tempo = None

    for line in out.splitlines():
        mm = _M_RE.match(line)
        if mm:
            cur_m = int(mm.group(1))
            cur_num = int(mm.group(2))
            cur_den = int(mm.group(3))
            cur_tempo = int(mm.group(4))
            tail = mm.group(5)
            mk = re.search(r"<(.+?)>", tail)
            measures[cur_m] = {
                "num": cur_num, "den": cur_den, "tempo": cur_tempo,
                "marker": mk.group(1) if mk else None,
            }
            continue

        bm = _B_RE.match(line)
        if bm and cur_m is not None:
            b = Beat()
            b.measure = cur_m
            b.voice = int(bm.group(1))
            b.tick = int(bm.group(3))
            b.gp_beat = (b.tick - TICK_ORIGIN) / float(TICKS_PER_QUARTER)
            b.num, b.den, b.tempo = cur_num, cur_den, cur_tempo
            payload = bm.group(5)
            if payload.strip().startswith("rest"):
                b.rest = True
            else:
                for nm in _NOTE_RE.finditer(payload):
                    flags = set(re.findall(r"\(([a-z]+)\)", nm.group(3)))
                    b.notes.append((int(nm.group(1)), int(nm.group(2)), flags))
            beats.append(b)

    # Record where each measure starts in gp_beat terms.  Time signatures can
    # change mid-song, so this is accumulated rather than computed as 4*(m-1).
    acc = 0.0
    for mnum in sorted(measures):
        measures[mnum]["gp_beat"] = acc
        acc += measures[mnum]["num"] * (4.0 / measures[mnum]["den"])
    return beats, measures


def pitch_class_vector(beats, track, gp_beat_lo, gp_beat_hi, sustain=True):
    """12-bin pitch-class histogram for notes sounding in [lo, hi).

    With sustain=True a note keeps contributing until the next onset in the same
    track, which approximates how the chord actually rings out."""
    pcv = [0.0] * 12
    onsets = [b for b in beats if not b.rest and b.notes]
    for i, b in enumerate(onsets):
        end = onsets[i + 1].gp_beat if (sustain and i + 1 < len(onsets)) else b.gp_beat + 0.25
        if end <= gp_beat_lo or b.gp_beat >= gp_beat_hi:
            continue
        overlap = min(end, gp_beat_hi) - max(b.gp_beat, gp_beat_lo)
        if overlap <= 0:
            continue
        for (s, f, flags) in b.notes:
            if "x" in flags:          # dead / muted: percussive, no pitch
                continue
            p = track.pitch(s, f)
            if p is None:
                continue
            pcv[p % 12] += overlap
    return pcv
