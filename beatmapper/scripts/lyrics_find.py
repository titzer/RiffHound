#!/usr/bin/env python3
"""Find candidate lyrics (synced and plain) for an audio track.

Usage: lyrics_find.py AUDIO [--title T] [--artist A] [--duration SECS]
                            [--max N] [--no-cache] [--check]

Title/artist/album/duration come from the file's tags (ffprobe), falling back
to the file name ("Artist - Title", BPM/key junk stripped); the options
override them.  Sources, queried serially: LRCLIB (/api/get, /api/search),
then NetEase (unofficial) when LRCLIB has no good synced match.

Prints to stdout, one record per line, tab separated:
    status<TAB>text
    query<TAB>source<TAB>artist<TAB>title<TAB>duration
    cand<TAB>rank<TAB>score<TAB>source_id<TAB>kind<TAB>artist<TAB>title<TAB>album
        <TAB>duration<TAB>n_lines<TAB>timed_path<TAB>txt_path
    warn<TAB>text
    done<TAB>number of cand lines
kind is synced|plain|instrumental.  txt_path holds the cleaned lyric lines
(no blank lines); timed_path (synced only) holds start<TAB>end<TAB>text per
line, end empty when unknown.  Both have the same text lines.
Exit codes: 0 candidates, 2 none, 3 not cached (--check), 1 failure.

Results are cached in ~/.cache/beatmapper/lyrics/<hash>/ keyed on the audio
file's path, size, mtime and the overrides; a later run (or --check) replays
results.tsv without touching the network.  Standard library only.
"""
import argparse
import difflib
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request

CACHE_ROOT = os.path.expanduser("~/.cache/beatmapper/lyrics")
UA = "beatmapper/0.1 (personal lyric mapping tool)"
LRCLIB = "https://lrclib.net/api"
NETEASE = "https://music.163.com/api"
REQ_TIMEOUT = 10.0
BUDGET = 20.0          # seconds of network time before we stop asking
KEEP = 20              # candidates written to the cache (--max picks from these)
MIN_GAP = 0.4         # seconds between requests to one host
SOURCE_PRIOR = {"lrclib": 1.0, "netease": 0.9}


# ---------------------------------------------------------------- output

def clean_field(v):
    if v is None:
        return ""
    return re.sub(r"[\t\r\n]+", " ", str(v)).strip()


def fmt_line(*fields):
    return "\t".join(clean_field(f) for f in fields)


def emit_raw(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def emit(*fields):
    emit_raw(fmt_line(*fields))


def log(msg):
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


# ---------------------------------------------------------------- cache

def cache_dir(audio, title, artist, duration):
    st = os.stat(audio)
    key = (f"{os.path.realpath(audio)}|{st.st_size}|{int(st.st_mtime)}|lyrics"
           f"|{title or ''}|{artist or ''}|{duration if duration is not None else ''}")
    return os.path.join(CACHE_ROOT, hashlib.sha1(key.encode()).hexdigest()[:24])


def write_atomic(path, text):
    d = os.path.dirname(path)
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".tmp-")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def replay_cache(d, max_n):
    """Re-emit cached records; returns the exit code, or None if not cached."""
    p = os.path.join(d, "results.tsv")
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8") as f:
        lines = [ln.rstrip("\n") for ln in f if ln.strip()]
    emit("status", "cached")
    n = 0
    for ln in lines:
        if ln.startswith("cand\t"):
            if n >= max_n:
                continue
            fields = ln.split("\t")
            if len(fields) >= 12 and not os.path.exists(fields[11]):
                return None   # cache damaged; recompute
            n += 1
        emit_raw(ln)
    emit("done", n)
    return 0 if n else 2


# ---------------------------------------------------------------- metadata

def find_tool(name):
    p = shutil.which(name)
    if p:
        return p
    for d in ("/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"):
        p = os.path.join(d, name)
        if os.access(p, os.X_OK):
            return p
    return None


def probe(audio):
    """(tags dict with lower-case keys, duration or None)."""
    ffprobe = find_tool("ffprobe")
    if not ffprobe:
        log("ffprobe not found")
        return {}, None
    try:
        out = subprocess.run([ffprobe, "-v", "error", "-show_entries", "format=duration:format_tags",
                              "-of", "json", audio], capture_output=True, text=True, timeout=20)
        info = json.loads(out.stdout or "{}").get("format", {})
    except Exception as e:  # noqa: BLE001
        log(f"ffprobe failed: {e!r}")
        return {}, None
    tags = {k.lower(): (v or "").strip() for k, v in (info.get("tags") or {}).items()}
    dur = None
    try:
        dur = float(info.get("duration"))
    except (TypeError, ValueError):
        pass
    return tags, dur


JUNK_RES = [
    r"\(\s*(official\s*(music\s*)?video|official\s*audio|lyrics?(\s*video)?|audio|hq|hd|live[^)]*|"
    r"remaster(ed)?[^)]*|\d{4}\s*remaster[^)]*|mono|stereo|demo|radio edit)\s*\)",
    r"\[\s*(official[^\]]*|lyrics?|audio|hq|hd|live[^\]]*|remaster[^\]]*)\s*\]",
    r"\s+-\s+(\d{4}\s+)?remaster(ed)?.*$",
    r"\s+-\s+live\b.*$",
    r"\b\d{2,3}\s*bpm\b",
    r"\bin\s+(?-i:[A-G])(#|b|\s*sharp|\s*flat)?\s*(m|min|minor|maj|major)?\s*$",
]
FEAT_RE = re.compile(r"\s*[\(\[]?\b(feat\.?|ft\.?|featuring)\s+[^\)\]]*[\)\]]?", re.I)


def clean_title(t):
    t = t or ""
    for rx in JUNK_RES:
        t = re.sub(rx, " ", t, flags=re.I)
    t = FEAT_RE.sub(" ", t)
    t = re.sub(r"\s+", " ", t).strip(" -_.")
    return t


def clean_artist(a):
    a = FEAT_RE.sub(" ", a or "")
    return re.sub(r"\s+", " ", a).strip(" -_.,")


def parse_filename(path):
    """(artist or '', title) guessed from the file name."""
    base = os.path.splitext(os.path.basename(path))[0]
    base = base.replace("_", " ")
    base = re.sub(r"\[[A-Za-z0-9_-]{11}\]", " ", base)          # YouTube ids
    base = re.sub(r"^\s*\d{1,3}[\s._-]+(?=\D)", "", base)        # track numbers
    parts = re.split(r"\s+[-–—]\s+", base, maxsplit=1)
    artist, title = ("", parts[0]) if len(parts) == 1 else (parts[0], parts[1])
    if not artist and "-" in title and " " not in title.strip():
        # "Blues-Am-Slow", "CrippleCreek-G-100": drop key/tempo tokens
        toks = [t for t in title.split("-") if t and not re.fullmatch(
            r"[A-G](#|b)?(m|min|maj)?|\d{2,3}(bpm)?|slow|fast|medium|uptempo", t, re.I)]
        title = " ".join(toks)
    title = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", title)           # CamelCase
    return clean_artist(artist), clean_title(title)


BACKING_RE = re.compile(r"backing\s*track|jam\s*track|karaoke|play[- ]?along|instrumental|"
                        r"\b\d{2,3}\s*bpm\b|\bin\s+(?-i:[A-G])(#|b)?\s*(m|min|minor|maj|major)?\s*$|"
                        r"-(?-i:[A-G])(#|b)?m?-|-\d{2,3}(-|$)", re.I)


def looks_like_backing(title, path):
    base = os.path.splitext(os.path.basename(path))[0]
    return bool(BACKING_RE.search(title) or BACKING_RE.search(base))


# ---------------------------------------------------------------- similarity

def norm(s):
    s = unicodedata.normalize("NFKC", s or "").casefold()
    s = s.replace("’", "'").replace("&", " and ")
    s = FEAT_RE.sub(" ", s)
    s = re.sub(r"\s*[\(\[][^\)\]]*[\)\]]\s*$", " ", s)
    s = re.sub(r"\s+-\s+.*remaster.*$", " ", s)
    s = "".join(c for c in unicodedata.normalize("NFKD", s) if not unicodedata.combining(c))
    s = re.sub(r"[^\w\s]", "", s)
    s = re.sub(r"\s+", " ", s).strip()
    s = re.sub(r"^the ", "", s)
    return s


def sim(a, b):
    na, nb = norm(a), norm(b)
    if not na or not nb:
        return 0.0
    r = difflib.SequenceMatcher(None, na, nb).ratio()
    ta, tb = set(na.split()), set(nb.split())
    small, big = (ta, tb) if len(ta) <= len(tb) else (tb, ta)
    if small and small <= big:
        r = max(r, 0.7 + 0.3 * len(small) / len(big))
    return r


# ---------------------------------------------------------------- lyric cleanup

SECTION_RE = re.compile(
    r"^\s*[\[\(\{]\s*(verse|chorus|pre[- ]?chorus|post[- ]?chorus|bridge|hook|intro|outro|refrain|"
    r"instrumental|interlude|solo|guitar solo|break|breakdown|repeat|x\s*\d+|\d+\s*x|coda|tag|"
    r"spoken|ad[- ]?lib)[^\]\)\}]*[\]\)\}]\s*", re.I)
CREDIT_RE = re.compile(
    r"^\s*(作词|作詞|作曲|编曲|編曲|制作人|製作人|混音|母带|和声|和聲|吉他|贝斯|鼓|监制|出品|词|曲|"
    r"lyrics\s+by|lyricist|composed\s+by|composer|written\s+by|produced\s+by|producer|arranged\s+by|"
    r"arranger|mixed\s+by|mastered\s+by)\s*[:：]", re.I)
META_RE = re.compile(r"^\s*\[(ar|ti|al|by|au|length|offset|re|ve|id|#)\s*:[^\]]*\]\s*$", re.I)
NOISE_ONLY_RE = re.compile(r"^[\s\W_♪♫♬🎵🎶]*$|^\s*[\(\[]?\s*(instrumental|music|musica|música|"
                           r"纯音乐，请欣赏|纯音乐|instrumental break)\s*[\)\]]?\s*$", re.I)
REPEAT_TAIL_RE = re.compile(r"\s+[\(\[]?\s*[x×]\s*\d+\s*[\)\]]?\s*$", re.I)


def clean_line(t):
    """Cleaned lyric text, or '' when the line carries no lyric."""
    t = unicodedata.normalize("NFC", t or "")
    t = re.sub(r"<\d+:\d+(?:[.:]\d+)?>", "", t)       # enhanced-LRC word stamps
    t = t.replace("\t", " ").strip()
    if not t or META_RE.match(t) or CREDIT_RE.match(t):
        return ""
    m = SECTION_RE.match(t)
    if m:
        t = t[m.end():].strip()
    t = REPEAT_TAIL_RE.sub("", t)
    t = re.sub(r"\s+", " ", t).strip()
    if NOISE_ONLY_RE.match(t):
        return ""
    return t


def clean_plain(text):
    out = []
    for ln in (text or "").replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        c = clean_line(re.sub(r"^(\[\d+:\d+(?:[.:]\d+)?\])+", "", ln.strip()))
        if c:
            out.append(c)
    return out


LRC_TS = re.compile(r"\[(\d+):(\d+(?:[.:]\d+)?)\]")


def parse_lrc(text):
    """[(start_s, text)] cleaned, sorted, offset applied."""
    offset = 0.0
    rows = []
    for ln in (text or "").replace("\r", "\n").split("\n"):
        m = re.match(r"^\s*\[offset\s*:\s*([+-]?\d+)\s*\]", ln, re.I)
        if m:
            offset = int(m.group(1)) / 1000.0
            continue
        stamps = []
        pos = 0
        ln = ln.strip()
        while True:
            m = LRC_TS.match(ln, pos)
            if not m:
                break
            stamps.append(int(m.group(1)) * 60 + float(m.group(2).replace(":", ".")))
            pos = m.end()
        if not stamps:
            continue
        body = clean_line(ln[pos:])
        if not body:
            continue
        for s in stamps:
            rows.append((s, body))
    # LRC offset: positive means lyrics come earlier
    rows = [(max(0.0, s - offset), b) for s, b in rows]
    rows.sort(key=lambda r: r[0])
    return rows


def yaml_scalar(v):
    v = v.strip()
    if v.startswith("'") and v.endswith("'") and len(v) >= 2:
        return v[1:-1].replace("''", "'")
    if v.startswith('"') and v.endswith('"') and len(v) >= 2:
        try:
            return json.loads(v)
        except ValueError:
            return v[1:-1]
    if v in ("~", "null"):
        return ""
    return v


def parse_lyricsfile(y):
    """Line list [(start_s, end_s or None, raw text)] from LRCLIB's YAML."""
    if not y:
        return []
    lines = y.replace("\r", "").split("\n")
    try:
        i = next(k for k, ln in enumerate(lines) if re.match(r"^lines\s*:\s*$", ln))
    except StopIteration:
        return []
    item_ind = None
    out, cur, last_key, key_ind = [], None, None, None
    for ln in lines[i + 1:]:
        if not ln.strip():
            continue
        ind = len(ln) - len(ln.lstrip(" "))
        if ind == 0 and not ln.startswith("-"):
            break                       # next top-level key
        s = ln.strip()
        if s.startswith("- ") and (item_ind is None or ind == item_ind):
            item_ind = ind
            cur = {}
            out.append(cur)
            s = s[2:]
            key_ind = ind + 2
        elif cur is None:
            continue
        elif ind > key_ind and last_key == "text" and not re.match(r"^[\w]+\s*:", s):
            cur["text"] = (cur.get("text", "") + " " + s).strip()   # wrapped scalar
            continue
        elif ind != key_ind:
            last_key = None
            continue                    # nested (words) or unknown
        m = re.match(r"^(\w+)\s*:\s*(.*)$", s)
        if not m:
            continue
        k, v = m.group(1), m.group(2)
        last_key = k
        if k in ("text", "start_ms", "end_ms"):
            cur[k] = yaml_scalar(v)
    res = []
    for c in out:
        try:
            st = float(c["start_ms"]) / 1000.0
        except (KeyError, ValueError):
            continue
        en = None
        try:
            en = float(c["end_ms"]) / 1000.0
        except (KeyError, ValueError):
            pass
        res.append((st, en, c.get("text", "")))
    res.sort(key=lambda r: r[0])
    return res


# ---------------------------------------------------------------- HTTP

class Net:
    def __init__(self):
        self.t0 = time.time()
        self.ok = 0
        self.failed = 0
        self.last_error = ""
        self.last_req = {}     # host -> time of the last request (pacing)

    def remaining(self):
        return BUDGET - (time.time() - self.t0)

    def get_json(self, url, headers=None, label=""):
        """Parsed JSON, or None on 404/other failure."""
        if self.remaining() <= 0.5:
            log(f"budget exhausted, skipping {url}")
            return None
        hdrs = {"User-Agent": UA, "Accept": "application/json"}
        hdrs.update(headers or {})
        host = urllib.parse.urlsplit(url).netloc
        for attempt in range(2):
            gap = time.time() - self.last_req.get(host, 0)
            if gap < MIN_GAP:
                time.sleep(MIN_GAP - gap)       # serial and unhurried
            t = self.last_req[host] = time.time()
            try:
                req = urllib.request.Request(url, headers=hdrs)
                with urllib.request.urlopen(req, timeout=min(REQ_TIMEOUT, max(1.0, self.remaining()))) as r:
                    data = r.read()
                self.ok += 1
                log(f"GET {url} -> 200 ({time.time() - t:.2f}s)")
                return json.loads(data.decode("utf-8", "replace"))
            except urllib.error.HTTPError as e:
                log(f"GET {url} -> HTTP {e.code} ({time.time() - t:.2f}s)")
                if e.code == 404:
                    self.ok += 1
                    return None
                if e.code in (429, 503) and attempt == 0:
                    try:
                        wait = float(e.headers.get("Retry-After") or 1)
                    except ValueError:
                        wait = 1.0
                    wait = min(max(wait, 2.0), 5.0)   # Retry-After is often 1; give it a bit more
                    if wait < self.remaining() - 1:
                        time.sleep(wait)
                        continue
                self.failed += 1
                self.last_error = f"{label or url}: HTTP {e.code}"
                return None
            except Exception as e:  # noqa: BLE001  (URLError, timeout, bad JSON)
                log(f"GET {url} failed: {e!r}")
                self.failed += 1
                self.last_error = f"{label or url}: {getattr(e, 'reason', e)}"
                return None
        return None


# ---------------------------------------------------------------- candidates

class Cand:
    def __init__(self, source, sid, artist, title, album, duration):
        self.source, self.sid = source, sid
        self.artist, self.title, self.album = artist or "", title or "", album or ""
        self.duration = duration
        self.timed = []      # [(start, end or None, text)]
        self.lines = []      # cleaned text lines
        self.instrumental = False
        self.score = 0.0
        self.dupes = 0
        self.reject = ""
        self.key = ""

    @property
    def kind(self):
        if self.instrumental:
            return "instrumental"
        return "synced" if self.timed else "plain"


def build_from_text(c, synced, plain, lyricsfile=None):
    rows = parse_lrc(synced) if synced else []
    if len(rows) >= 3:
        timed = [(s, None, t) for s, t in rows]
        yl = parse_lyricsfile(lyricsfile) if lyricsfile else []
        if yl:
            ycl = [(s, e, clean_line(t)) for s, e, t in yl]
            ycl = [r for r in ycl if r[2]]
            if len(ycl) == len(timed) and all(
                    abs(a[0] - b[0]) < 0.05 and a[2] == b[2] for a, b in zip(ycl, timed)):
                timed = ycl
            elif len(ycl) >= 3:
                log(f"{c.sid}: lyricsfile disagrees with LRC; using LRC starts only")
        c.timed = timed
        c.lines = [t for _, _, t in timed]
    else:
        c.lines = clean_plain(plain or synced or "")
    return c


def lrclib_cand(rec):
    c = Cand("lrclib", f"lrclib:{rec.get('id')}", rec.get("artistName"), rec.get("trackName"),
             rec.get("albumName"), rec.get("duration"))
    if rec.get("instrumental"):
        c.instrumental = True
        return c
    build_from_text(c, rec.get("syncedLyrics"), rec.get("plainLyrics"), rec.get("lyricsfile"))
    if not c.lines:
        c.instrumental = True
    return c


def dur_score(qd, cd):
    if not qd or not cd:
        return 0.5
    d = abs(qd - cd)
    if d <= 2:
        return 1.0
    zero = max(10.0, 0.05 * qd)
    return max(0.0, 1.0 - (d - 2) / (zero - 2))


def score(c, q):
    title_sim = sim(q["title"], c.title)
    artist_sim = sim(q["artist"], c.artist) if q["artist"] else 0.75
    c.title_sim = title_sim
    qd = q["duration"]
    c.reject = ""
    if title_sim < (0.95 if q.get("strict") else 0.6):
        c.reject = f"title mismatch ({title_sim:.2f})"
    elif qd and c.duration and abs(qd - c.duration) > 0.10 * qd:
        c.reject = f"duration {c.duration:.0f}s vs {qd:.0f}s"
    elif qd and c.timed and c.timed[-1][0] > qd + 5:
        c.reject = f"LRC ends at {c.timed[-1][0]:.0f}s, track is {qd:.0f}s"
    s = (0.35 * title_sim + 0.20 * artist_sim + 0.25 * dur_score(qd, c.duration)
         + (0.15 if c.timed else 0.0))
    s *= SOURCE_PRIOR.get(c.source, 0.8)
    if c.instrumental:
        s *= 0.5
    c.score = min(1.0, s)
    return c


def text_key(lines):
    return re.sub(r"[^a-z]", "", " ".join(lines).casefold())[:600]


def cluster(cands):
    """Keep the best of each group of near-identical texts; return them sorted."""
    cands = sorted(cands, key=lambda c: -c.score)
    reps = []
    for c in cands:
        c.key = text_key(c.lines) if not c.instrumental else "#instrumental"
        for r in reps:
            if (r.instrumental == c.instrumental and
                    (r.key == c.key or (r.key and c.key and
                     difflib.SequenceMatcher(None, r.key, c.key).quick_ratio() >= 0.92 and
                     difflib.SequenceMatcher(None, r.key, c.key).ratio() >= 0.92))):
                r.dupes += 1
                r.cluster_sources.add(c.source)
                break
        else:
            c.cluster_sources = {c.source}
            reps.append(c)
    for r in reps:
        # many uploads of one text = the canonical version; other sources agreeing helps too
        r.score = min(1.0, r.score + 0.05 * min(1.0, math.log10(1 + r.dupes))
                      + (0.03 if len(r.cluster_sources) > 1 else 0.0))
    reps.sort(key=lambda c: -c.score)
    return reps


# ---------------------------------------------------------------- sources

def lrclib_search(net, params, label):
    url = f"{LRCLIB}/search?" + urllib.parse.urlencode(params)
    res = net.get_json(url, label=label)
    return res if isinstance(res, list) else []


def query_lrclib(net, q, found):
    """Fill found (sid -> Cand). Returns quickly once there are enough good hits."""
    def add(recs):
        for rec in recs:
            if not isinstance(rec, dict) or rec.get("id") is None:
                continue
            sid = f"lrclib:{rec['id']}"
            if sid not in found:
                found[sid] = score(lrclib_cand(rec), q)

    def good():
        return sum(1 for c in found.values()
                   if not c.reject and c.timed and c.title_sim >= 0.8 and c.score >= 0.75)

    title, artist, dur = q["title"], q["artist"], q["duration"]
    if title and artist:
        emit("status", "searching lrclib")
        params = {"track_name": title, "artist_name": artist}
        if dur:
            params["duration"] = str(int(round(dur)))
        rec = net.get_json(f"{LRCLIB}/get?" + urllib.parse.urlencode(params), label="lrclib get")
        if isinstance(rec, dict) and rec.get("id") is not None:
            add([rec])
        add(lrclib_search(net, {"track_name": title, "artist_name": artist}, "lrclib search"))
    if good() >= 3:
        return
    emit("status", "searching lrclib (free text)")
    titles = [title]
    bare = re.sub(r"\s*[\(\[][^\)\]]*[\)\]]\s*", " ", title).strip()
    if bare and bare != title:
        titles.append(bare)
    if ":" in bare:
        titles.append(bare.split(":")[0].strip())
    seen = set()
    for t in titles:
        for qs in ([f"{artist} {t}"] if artist else []) + [t]:
            if qs in seen or not qs.strip():
                continue
            seen.add(qs)
            add(lrclib_search(net, {"q": qs}, "lrclib q search"))
            if good() >= 3:
                return


def query_netease(net, q, found):
    emit("status", "searching netease")
    hdrs = {"Referer": "https://music.163.com", "User-Agent": "Mozilla/5.0 (compatible; " + UA + ")"}
    s = f"{q['title']} {q['artist']}".strip()
    res = net.get_json(f"{NETEASE}/search/get?" + urllib.parse.urlencode(
        {"s": s, "type": 1, "limit": 15}), headers=hdrs, label="netease search")
    songs = (((res or {}).get("result") or {}).get("songs")) or []
    picks = []
    for sg in songs:
        try:
            d = float(sg.get("duration") or 0) / 1000.0 or None
        except (TypeError, ValueError):
            d = None
        artist = ", ".join(a.get("name", "") for a in sg.get("artists") or [])
        c = Cand("netease", f"netease:{sg.get('id')}", artist, sg.get("name"),
                 (sg.get("album") or {}).get("name"), d)
        if c.sid in found:
            continue
        score(c, q)       # pre-score on metadata alone
        if c.reject or c.title_sim < 0.7:
            continue
        if q["duration"] and d and abs(d - q["duration"]) > max(3.0, 0.03 * q["duration"]):
            continue
        picks.append(c)
    picks.sort(key=lambda c: -c.score)
    for c in picks[:2]:
        lr = net.get_json(f"{NETEASE}/song/lyric?" + urllib.parse.urlencode(
            {"id": c.sid.split(":", 1)[1], "lv": 1, "tv": -1}), headers=hdrs, label="netease lyric")
        text = ((lr or {}).get("lrc") or {}).get("lyric") or ""
        if not text.strip():
            continue
        if "纯音乐" in text and len(clean_plain(text)) <= 1:
            c.instrumental = True
        else:
            build_from_text(c, text, text)
            if not c.lines:
                continue
        found[c.sid] = score(c, q)


# ---------------------------------------------------------------- main

def write_results(d, ranked, header_lines, final=True):
    os.makedirs(d, exist_ok=True)
    out = list(header_lines)
    for i, c in enumerate(ranked, 1):
        txt = os.path.join(d, f"{i}.txt")
        write_atomic(txt, "".join(clean_field(t) + "\n" for t in c.lines))
        tsv = ""
        if c.timed:
            tsv = os.path.join(d, f"{i}.tsv")
            write_atomic(tsv, "".join(
                f"{s:.2f}\t{'' if e is None else f'{e:.2f}'}\t{clean_field(t)}\n" for s, e, t in c.timed))
        else:
            stale = os.path.join(d, f"{i}.tsv")
            if os.path.exists(stale):
                os.unlink(stale)
        out.append(fmt_line("cand", i, f"{c.score:.3f}", c.sid, c.kind, c.artist, c.title, c.album,
                            "" if c.duration is None else f"{float(c.duration):.1f}",
                            len(c.lines), tsv, txt))
    # drop numbered files from an earlier, longer result list (only our own names)
    for name in os.listdir(d):
        m = re.fullmatch(r"(\d+)\.(txt|tsv)", name)
        if m and int(m.group(1)) > len(ranked):
            os.unlink(os.path.join(d, name))
    res = os.path.join(d, "results.tsv")
    if final:
        write_atomic(res, "".join(ln + "\n" for ln in out))
    elif os.path.exists(res):
        os.unlink(res)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("audio")
    ap.add_argument("--title")
    ap.add_argument("--artist")
    ap.add_argument("--duration", type=float)
    ap.add_argument("--max", type=int, default=8)
    ap.add_argument("--no-cache", action="store_true")
    ap.add_argument("--check", action="store_true", help="only answer from the cache")
    a = ap.parse_args()
    if not os.path.exists(a.audio):
        emit("warn", f"no such file: {a.audio}")
        return 1
    d = cache_dir(a.audio, a.title, a.artist, a.duration)
    if a.check or not a.no_cache:
        rc = replay_cache(d, max(0, a.max))
        if rc is not None:
            return rc
        if a.check:
            return 3

    t0 = time.time()
    emit("status", "reading tags")
    tags, fdur = probe(a.audio)
    fa, ft = parse_filename(a.audio)
    src = []
    title = clean_title(tags.get("title", ""))
    artist = clean_artist(tags.get("artist") or tags.get("album_artist") or "")
    if title:
        src.append("tags")
    else:
        title = ft
        if not artist:
            artist = fa
        src.append("filename")
    if not artist and fa:
        artist = fa
    if a.title:
        title = a.title.strip()
    if a.artist is not None:
        artist = a.artist.strip()
    duration = a.duration if a.duration else fdur
    if a.title or a.artist is not None or a.duration:
        src.append("args")
    backing = looks_like_backing(title, a.audio) and not a.title
    q = {"title": title, "artist": artist, "album": tags.get("album", ""), "duration": duration,
         "strict": backing}
    header = [fmt_line("query", "+".join(src), artist, title,
                       "" if duration is None else f"{duration:.1f}")]
    emit_raw(header[0])
    if backing:
        w = fmt_line("warn", "looks like a backing track; only exact title matches are kept")
        header.append(w)
        emit_raw(w)
    if not title:
        emit("warn", "no title in tags or file name")
        emit("done", 0)
        return 2

    net = Net()
    found = {}
    query_lrclib(net, q, found)
    best_synced = max((c.score for c in found.values() if not c.reject and c.timed), default=0)
    if best_synced < 0.75 and not backing:
        query_netease(net, q, found)

    if net.ok == 0 and net.failed:
        emit("warn", f"network failure: {net.last_error}")
        emit("done", 0)
        return 1
    if net.failed:
        emit("warn", f"some requests failed: {net.last_error}")   # live only, not replayed

    cands = list(found.values())
    for c in cands:
        if c.reject:
            log(f"reject {c.sid} {c.artist} / {c.title} ({c.duration}): {c.reject}")
    ok = [c for c in cands if not c.reject]
    lyr = [c for c in ok if not c.instrumental]
    if not lyr and not backing:
        # No version fits the track length: offer the texts (untimed) of the
        # right song at a lower score rather than nothing.
        relaxed = [c for c in cands if not c.instrumental and c.reject
                   and c.title_sim >= 0.8 and not c.reject.startswith("title")
                   and (not q["artist"] or sim(q["artist"], c.artist) >= 0.7)]
        if relaxed:
            for c in relaxed:
                c.timed = []
                c.score *= 0.6
            lyr = relaxed
            w = fmt_line("warn", "no version matches the track length; timings dropped")
            header.append(w)
            emit_raw(w)
    if lyr:
        ranked = cluster(lyr)
    else:
        ranked = cluster([c for c in ok if c.instrumental])[:1]
    ranked = ranked[:KEEP]
    for c in ranked:
        log(f"{c.score:.3f} {c.sid} {c.kind} {c.artist} / {c.title} ({c.duration}) "
            f"lines={len(c.lines)} dupes={c.dupes} sources={','.join(sorted(c.cluster_sources))}")

    emit("status", "writing results")
    try:
        # an incomplete answer (requests failed, nothing solid found) is not
        # cached, so the next run asks again
        final = not net.failed or (ranked and ranked[0].timed and ranked[0].score >= 0.9)
        out = write_results(d, ranked, header, final)
        if not final:
            emit("warn", "results incomplete; not cached")
    except OSError as e:
        emit("warn", f"cannot write cache: {e}")
        return 1
    n = 0
    for ln in out:
        if ln.startswith("cand\t") and n < a.max:
            emit_raw(ln)
            n += 1
    log(f"lyrics lookup took {time.time() - t0:.1f}s")
    emit("done", n)
    return 0 if n else 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(1)
    except Exception as e:  # noqa: BLE001
        emit("warn", f"internal error: {e!r}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
