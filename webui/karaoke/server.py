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
import json
import mimetypes
import re
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

MEDIA_EXT = {".mp3", ".m4a", ".ogg", ".wav", ".flac", ".mp4", ".m4v", ".mov", ".webm"}
RESCAN_AFTER = 5.0          # seconds; a listing this stale is re-walked
PAGE = "v0.2.html"


# --- the library ----------------------------------------------------------

class Library:
    """The roots, walked on demand and remembered for a few seconds.

    Rescanning per request keeps the listing honest while you are editing
    charts next door, which is the whole point of running this beside the
    beatmapper; caching it for RESCAN_AFTER keeps a phone hammering the search
    box from walking the disk once per keystroke.
    """

    def __init__(self, roots):
        self.roots = [Path(r).expanduser().resolve() for r in roots]
        self.lock = threading.Lock()
        self.tracks = {}        # id -> dict
        self.by_path = {}       # resolved media path -> id
        self.scanned = 0.0

    def scan(self, force=False):
        with self.lock:
            if not force and time.time() - self.scanned < RESCAN_AFTER:
                return
            tracks, by_path = {}, {}
            for i, root in enumerate(self.roots):
                for path in sorted(root.rglob("*")):
                    if not path.is_file() or path.suffix.lower() not in MEDIA_EXT:
                        continue
                    rel = path.relative_to(root)
                    tid = f"{i}/{rel.with_suffix('').as_posix()}"
                    chart = path.with_suffix(".txt")
                    st = path.stat()
                    tracks[tid] = {
                        "id": tid,
                        "title": re.sub(r"[_\-]+", " ", rel.stem).strip(),
                        "source": root.name,
                        "media": path.name,
                        "chart": chart.is_file(),
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
    Handler.library = Library(args.roots)
    Handler.page = page
    Handler.quiet = args.quiet
    Handler.touch = staticmethod(lambda: last.__setitem__(0, time.time()))

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.daemon_threads = True
    host = args.host if args.host not in ("0.0.0.0", "::") else socket.gethostname()
    url = "http://%s:%d" % (host, httpd.server_address[1])

    Handler.library.scan()
    print("%s  -- %d tracks in %s" %
          (url, len(Handler.library.tracks),
           ", ".join(str(r) for r in Handler.library.roots)), flush=True)
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
