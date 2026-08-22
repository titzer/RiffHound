#!/usr/bin/env python3
"""RiffHound karaoke library server.

Serves one or more folders of tracks over HTTP so that the karaoke client --
or a phone on a music stand, or curl -- can list them, search them, and play
them without touching the filesystem.  A track is a media file; the beatmap is
the .txt of the same stem beside it, and is optional.

    ./server.py ~/Music/practice ~/Music/setlist
    ./server.py --port 9000 --host 0.0.0.0 ~/Music

The page is served from this same directory, so the client it hands out is
same-origin with the API it then calls: no CORS to arrange, no file:// limits,
and the browser's own cache does the caching.

Endpoints
---------
GET /                        the karaoke page
GET /api/hello               { riffhound, name, roots, tracks } -- the handshake
GET /api/tracks[?q=]         { tracks: [...] }, filtered by a subsequence match
GET /api/resolve?path=ABS    the id of that file, as text/plain (404 if unknown)
GET /media/<id>              the audio, with Range and ETag
GET /chart/<id>              the beatmap .txt, with ETag

Folders that are not part of the library -- originals/, stems/, anything whose
contents mirror what is already there -- are left out two ways: a .riffignore
file inside the folder, which travels with the library and so holds for every
client and every way of starting the server, or --exclude on the command line
for someone who would rather not write to the library at all.  Hidden files and
folders are always skipped.

An id is "<root index>/<path under that root, without extension>", which is
readable, debuggable, and stable as long as the file stays where it is.  It is
deliberately not a content hash: hashing every file to list a folder is a cost
with no payer yet, and swapping it in later changes this one function plus the
ETags, not the protocol.

Caching is the boring kind on purpose.  Every file response carries an ETag of
its size and mtime and honours If-None-Match, so a reload costs one 304 per
file and the client stores nothing it has to invalidate.  Content-hash ids
would let a client keep a copy across renames and edits; that is the
conversation this leaves room for rather than pre-empts.
"""

import argparse
import fnmatch
import json
import mimetypes
import os
import re
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

# Ordered, because the order is a preference: two files with the same stem are
# one track -- song.mp3 and song.m4a are the same song, not two of them -- and
# the one served should be the same one every time rather than whichever the
# directory walk happened to reach last.
MEDIA_EXT = (".mp3", ".m4a", ".ogg", ".webm", ".flac", ".wav",
             ".mp4", ".m4v", ".mov")
RESCAN_AFTER = 5.0          # seconds; a listing this stale is re-walked
PAGE = "v0.3.html"

# A folder holding this file is not part of the library.  A library that keeps
# its untouched copies in originals/ has every track twice -- once to play and
# once that is not for playing -- and no amount of looking at the names can tell
# which is which.  The marker travels with the folder, so it holds for every
# client and every way of starting the server; --exclude is the same rule for
# someone who would rather not write to the library at all.
IGNORE_FILE = ".riffignore"


# --- charts ---------------------------------------------------------------

# Memoised on (size, mtime): the chart being edited next door is re-read, the
# other four hundred are not.  Without this a listing walks every sidecar every
# five seconds, which is the kind of cost that only shows up on someone else's
# library.
_flags = {}


def chart_flags(path):
    """What kinds of annotation a chart carries.

    So that a client can filter a listing -- "only tracks I have beatmapped" --
    without fetching every chart in it.  The keywords are the ones in
    format-spec.md; a reader that does not know an event ignores it, which is
    why an unknown line counts for nothing rather than for something.
    """
    try:
        st = path.stat()
    except OSError:
        return {"beats": False, "chords": False, "lyrics": False, "loops": False}

    key = (str(path), st.st_size, st.st_mtime_ns)
    hit = _flags.get(str(path))
    if hit and hit[0] == key:
        return hit[1]

    has = {"beats": False, "chords": False, "lyrics": False, "loops": False}
    try:
        with path.open("r", errors="replace") as f:
            for line in f:
                line = re.sub(r"(^|[ \t])#.*", "", line).strip()
                parts = line.split(None, 2)
                if len(parts) < 3:
                    continue
                tok = parts[2].split(None, 1)[0]
                if tok in ("B", "BM") or tok.startswith(("Bx", "BMx")):
                    has["beats"] = True
                elif tok in ("chord:", "chords:"):
                    has["chords"] = True
                elif tok in ("lyric:", "lyric"):
                    has["lyrics"] = True
                elif tok.startswith("loop:"):
                    has["loops"] = True
                if all(has.values()):
                    break
    except OSError:
        pass

    _flags[str(path)] = (key, has)
    return has


# --- the library ----------------------------------------------------------

class Library:
    """The roots, walked on demand and remembered for a few seconds.

    Rescanning per request keeps the listing honest while you are editing
    charts next door, which is the whole point of running this beside the
    beatmapper; caching it for RESCAN_AFTER keeps a phone hammering the search
    box from walking the disk once per keystroke.
    """

    def __init__(self, roots, exclude=()):
        self.roots = dedupe_roots(Path(r).expanduser().resolve() for r in roots)
        self.exclude = list(exclude)
        self.lock = threading.Lock()
        self.tracks = {}        # id -> dict
        self.by_path = {}       # resolved media path -> id
        self.scanned = 0.0

    def skip(self, root, d):
        """Is this folder outside the library?"""
        if d.name.startswith("."):
            return True
        if (d / IGNORE_FILE).is_file():
            return True
        rel = d.relative_to(root).as_posix()
        return any(fnmatch.fnmatch(d.name, g) or fnmatch.fnmatch(rel, g)
                   for g in self.exclude)

    def scan(self, force=False):
        with self.lock:
            if not force and time.time() - self.scanned < RESCAN_AFTER:
                return
            tracks, by_path = {}, {}
            for i, root in enumerate(self.roots):
                for dirpath, dirnames, filenames in os.walk(root):
                    here = Path(dirpath)
                    # Pruned in place, so os.walk does not descend into it: an
                    # excluded tree costs one readdir rather than a walk of
                    # everything underneath.
                    dirnames[:] = sorted(d for d in dirnames
                                         if not self.skip(root, here / d))
                    for name in sorted(filenames):
                        if name.startswith("."):
                            continue
                        path = here / name
                        if path.suffix.lower() not in MEDIA_EXT:
                            continue

                        rel = path.relative_to(root)
                        tid = f"{i}/{rel.with_suffix('').as_posix()}"

                        # Same stem, another extension: one track, and the
                        # better encoding wins whichever order they were found.
                        old = tracks.get(tid)
                        if old and (MEDIA_EXT.index(Path(old["media"]).suffix.lower())
                                    <= MEDIA_EXT.index(path.suffix.lower())):
                            continue

                        chart = path.with_suffix(".txt")
                        st = path.stat()
                        folder = rel.parent.as_posix()
                        tracks[tid] = {
                            "id": tid,
                            "title": re.sub(r"[_\-]+", " ", rel.stem).strip(),
                            "source": root.name,
                            # The folder under the root, which is the only thing
                            # that tells two files of the same name apart -- a
                            # library with the same hymn in two folders is a
                            # library, not a mistake.
                            "folder": "" if folder == "." else folder,
                            "media": path.name,
                            "chart": chart.is_file(),
                            "has": chart_flags(chart) if chart.is_file()
                                   else {"beats": False, "chords": False,
                                         "lyrics": False, "loops": False},
                            "bytes": st.st_size,
                            "mtime": int(st.st_mtime),
                        }
                        by_path[str(path)] = tid
            self.tracks, self.by_path, self.scanned = tracks, by_path, time.time()

    def list(self, q=None):
        self.scan()
        rows = list(self.tracks.values())
        if q:
            rows = [t for t in rows
                    if fuzzy(q, t["title"]) or fuzzy(q, t["source"])]
        return sorted(rows, key=lambda t: (t["source"], t["title"]))

    def media_path(self, tid):
        """The media file for an id, or None -- never a path outside a root."""
        self.scan()
        t = self.tracks.get(tid)
        if not t:
            return None
        i, _, rel = tid.partition("/")
        root = self.roots[int(i)]
        path = (root / rel).with_suffix(Path(t["media"]).suffix)
        # The id came out of our own scan, so this cannot escape; check anyway,
        # because the day it can is the day someone else builds the id.
        try:
            path.resolve().relative_to(root)
        except ValueError:
            return None
        return path if path.is_file() else None

    def chart_path(self, tid):
        media = self.media_path(tid)
        if not media:
            return None
        chart = media.with_suffix(".txt")
        return chart if chart.is_file() else None

    def resolve(self, path):
        """The id of a file named by absolute path, or None if it is not ours."""
        self.scan(force=True)
        try:
            return self.by_path.get(str(Path(path).expanduser().resolve()))
        except OSError:
            return None


def lan_address():
    """The address of the interface that would carry traffic off this machine.

    Opens a UDP socket and sends nothing: connect() on a datagram socket only
    consults the routing table, which is the only thing that knows which of
    several interfaces a phone across the room would arrive on.  Guessing from
    gethostname() gets you 127.0.0.1 on a machine with a tidy /etc/hosts.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return socket.gethostname()
    finally:
        s.close()


def dedupe_roots(roots):
    """Distinct roots, none of them inside another.

    Serving a folder and one of its subfolders lists everything in the
    subfolder twice, under two ids, because nothing downstream can tell that
    two paths are the same file.  The subfolder is already covered, so it goes.
    """
    out = []
    for r in sorted(set(roots), key=lambda p: len(p.parts)):
        if any(r == k or k in r.parents for k in out):
            sys.stderr.write("[server] %s is already inside %s; skipping it\n" % (r, out[-1]))
            continue
        out.append(r)
    return out


def fuzzy(q, s):
    """Every letter of the query in order -- the same match the client uses."""
    q, s, qi = q.lower(), s.lower(), 0
    for ch in s:
        if qi < len(q) and ch == q[qi]:
            qi += 1
    return qi == len(q)


# --- the server -----------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "riffhound/1"
    protocol_version = "HTTP/1.1"

    # set on the class by main()
    library = None
    page = None
    quiet = False
    touch = staticmethod(lambda: None)

    def log_message(self, fmt, *args):
        if not self.quiet:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    # -- replies -----------------------------------------------------------

    def head(self, status, ctype, length, extra=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        # Anything may ask: the threat model here is "everyone in the room",
        # and a client served from somewhere else still has to be able to look.
        self.send_header("Access-Control-Allow-Origin", "*")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()

    def send_bytes(self, body, ctype, status=200, extra=None):
        self.head(status, ctype, len(body), extra)
        if self.command != "HEAD":
            self.wfile.write(body)

    def send_json(self, obj, status=200):
        self.send_bytes(json.dumps(obj).encode(), "application/json", status,
                        {"Cache-Control": "no-store"})

    def send_text(self, text, status=200):
        self.send_bytes(text.encode(), "text/plain; charset=utf-8", status,
                        {"Cache-Control": "no-store"})

    def fail(self, status, why):
        self.send_text(why + "\n", status)

    def send_file(self, path, ctype=None):
        """A file, with ETag revalidation and Range -- both of which the audio
        element needs: Range to seek without refetching, ETag so returning to a
        track you played yesterday costs a 304 rather than the whole file."""
        st = path.stat()
        etag = '"%x-%x"' % (st.st_size, st.st_mtime_ns)
        ctype = ctype or mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        common = {"ETag": etag, "Accept-Ranges": "bytes", "Cache-Control": "no-cache"}

        if self.headers.get("If-None-Match") == etag:
            self.head(304, ctype, 0, common)
            return

        start, end = 0, st.st_size - 1
        rng = self.headers.get("Range", "")
        m = re.match(r"bytes=(\d*)-(\d*)$", rng.strip()) if rng else None
        partial = False
        if m and st.st_size:
            g0, g1 = m.group(1), m.group(2)
            if g0:
                start = int(g0)
                if g1:
                    end = min(int(g1), st.st_size - 1)
            elif g1:                      # bytes=-N is the last N bytes
                start = max(0, st.st_size - int(g1))
            partial = True

        # A start past the end is unsatisfiable, not the last byte: clamping it
        # would hand back one byte and call it success.
        if partial and (start >= st.st_size or start > end):
            self.head(416, ctype, 0, {**common,
                                      "Content-Range": "bytes */%d" % st.st_size})
            return

        n = end - start + 1
        if partial:
            common["Content-Range"] = "bytes %d-%d/%d" % (start, end, st.st_size)
        self.head(206 if partial else 200, ctype, n, common)
        if self.command == "HEAD":
            return
        with path.open("rb") as f:
            f.seek(start)
            left = n
            while left > 0:
                chunk = f.read(min(1 << 16, left))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)

    # -- routing -----------------------------------------------------------

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        self.touch()
        u = urlparse(self.path)
        path = unquote(u.path)
        query = parse_qs(u.query)
        lib = self.library

        try:
            if path in ("/", "/index.html", "/karaoke"):
                return self.send_file(self.page, "text/html; charset=utf-8")

            if path == "/api/hello":
                lib.scan()
                return self.send_json({
                    "riffhound": 1,
                    "name": socket.gethostname(),
                    "roots": [r.name for r in lib.roots],
                    "tracks": len(lib.tracks),
                })

            if path == "/api/tracks":
                q = (query.get("q") or [""])[0].strip()
                return self.send_json({"tracks": lib.list(q or None)})

            if path == "/api/resolve":
                want = (query.get("path") or [""])[0]
                tid = lib.resolve(want) if want else None
                return self.send_text(tid) if tid else self.fail(404, "not in any root")

            for prefix, find in (("/media/", lib.media_path), ("/chart/", lib.chart_path)):
                if path.startswith(prefix):
                    got = find(path[len(prefix):])
                    if not got:
                        return self.fail(404, "no such track")
                    return self.send_file(
                        got, "text/plain; charset=utf-8" if prefix == "/chart/" else None)

            return self.fail(404, "no such endpoint")
        except BrokenPipeError:
            pass                    # the audio element hung up mid-seek; normal
        except Exception as e:      # never take the server down over one request
            self.log_message("error: %s", e)
            try:
                self.fail(500, str(e))
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("roots", nargs="+", help="folders of tracks to serve")
    ap.add_argument("--host", default="127.0.0.1",
                    help="0.0.0.0 to let the rest of the room in (default: %(default)s)")
    ap.add_argument("--port", type=int, default=8177,
                    help="0 picks a free one (default: %(default)s)")
    ap.add_argument("--exclude", action="append", default=[], metavar="GLOB",
                    help="skip folders matching this, by name or by path under "
                         "the root; repeatable (e.g. --exclude originals)")
    ap.add_argument("--page", default=None, help="the client to serve at /")
    ap.add_argument("--url-file", default=None,
                    help="write the server's URL here once it is listening")
    ap.add_argument("--idle-exit", type=float, default=0,
                    help="quit after this many seconds with no requests")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    for r in args.roots:
        if not Path(r).expanduser().is_dir():
            sys.exit("not a folder: %s" % r)

    page = Path(args.page) if args.page else Path(__file__).resolve().parent / PAGE
    if not page.is_file():
        sys.exit("no page to serve at %s (pass --page)" % page)

    last = [time.time()]
    Handler.library = Library(args.roots, args.exclude)
    Handler.page = page
    Handler.quiet = args.quiet
    Handler.touch = staticmethod(lambda: last.__setitem__(0, time.time()))

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.daemon_threads = True
    port = httpd.server_address[1]
    loopback = args.host in ("127.0.0.1", "localhost", "::1")
    url = "http://%s:%d" % ("127.0.0.1" if loopback else lan_address(), port)

    Handler.library.scan()
    print("%s  -- %d tracks in %s%s" %
          (url, len(Handler.library.tracks),
           ", ".join(str(r) for r in Handler.library.roots),
           "  (skipping %s)" % ", ".join(args.exclude) if args.exclude else ""),
          flush=True)
    # Bound to loopback, the address the phone on the music stand would use is
    # not merely absent from this line -- it does not work, and nothing about
    # "127.0.0.1" says why.  So say it here, where the question gets asked.
    if loopback:
        print("this machine only; --host 0.0.0.0 to let the rest of the room in "
              "(then http://%s:%d)" % (lan_address(), port), flush=True)
    if args.url_file:
        Path(args.url_file).write_text(url + "\n")

    if args.idle_exit > 0:
        # For the server riffplay starts behind you: it should not outlive the
        # browser tab that wanted it, and nothing else is going to tidy it up.
        def reaper():
            while time.time() - last[0] < args.idle_exit:
                time.sleep(min(5.0, args.idle_exit))
            httpd.shutdown()
        threading.Thread(target=reaper, daemon=True).start()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    httpd.server_close()


if __name__ == "__main__":
    main()
