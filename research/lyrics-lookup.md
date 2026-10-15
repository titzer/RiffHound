# Lyrics lookup for "Find lyrics"

How beatmapper could turn a track (title, artist, duration, file tags) into a
ranked list of candidate lyric texts, plain and timestamped (LRC), for the
Lyric Index tool. Researched 2026-10-11, with live probes of LRCLIB, lrcmux,
NetEase, MusicBrainz, Genius, lyrics.ovh, ChartLyrics and Happi. This report
shows response shapes only. Lyric text is cut to a line or two.

## 1. Recommendation

1. **Primary source: LRCLIB** (`lrclib.net/api`). It is free, needs no key, is
   built for machines, and returns *both* `plainLyrics` and `syncedLyrics` for
   each hit, plus duration, so ranking is easy. Coverage of mainstream Western
   music is very good. A search for "Bohemian Rhapsody / Queen" returned 20
   records, all with synced lyrics.
2. **Second synced source: NetEase Cloud Music** (unofficial JSON API, no key).
   Strong for Chinese, Japanese and Korean music, and has good Western coverage
   too. It returns LRC plus optional translation. Strip the credit lines it
   prepends (`作词 : …`, `作曲 : …`).
3. **Optional plain-text fallback: Genius** (official search API with a free
   token, then page scraping for the text). Use it only when 1 and 2 find
   nothing, and keep it off by default or behind a setting, because scraping
   breaks Genius's ToS (see section 8).
4. **Identification (when tags are missing or messy):** parse tags and the
   filename first. If that fails, fingerprint with **Chromaprint `fpcalc` →
   AcoustID → MusicBrainz recording** to get a canonical title, artist and
   length, then run the lookup above.
5. **Skip:** Musixmatch official API (the free tier returns only a ~30% preview
   and is non-commercial), lyrics.ovh (returned 404 "No lyrics found" for
   Bohemian Rhapsody), ChartLyrics (shut down), Happi (502), Spotify and Apple
   lyrics (no public API, and ToS forbid it), and AZLyrics or Lyrics.com
   scraping (ToS, bot blocking, and plain text only, which Genius already
   covers).

**Use a Python sidecar, not native libcurl, at least at first.** It matches the
existing `stems_demucs.py` pattern (spawned with posix_spawn, tab-separated
stdout). HTML scraping and fuzzy matching (`rapidfuzz`, BeautifulSoup) are much
easier to change in Python. Lookups take about 0.3–2 s, so speed doesn't
matter. The venv can be tiny (`requests`, `rapidfuzz`, optional `pyacoustid`,
`beautifulsoup4`) and doesn't need torch, so give it its own venv
(`external/venv-lyrics`) or reuse a system Python 3. If the feature later
becomes LRCLIB-only, a native libcurl + JSON call is about 100 lines and would
remove the Python dependency. The ranking logic below is simple enough to port.

Do not build on the `syncedlyrics` package as-is. It is unmaintained (last
release 1.0.1, 2024-07), returns only the *first* hit as a single string instead
of a ranked list, and several of its providers are broken. Use it as a reference
for provider endpoints only.

## 2. Comparison

| Source | Synced? | Key | Limits | Status (probed 2026-10) | Legal |
|---|---|---|---|---|---|
| **LRCLIB** | yes (LRC, plus new YAML "lyricsfile" with ms start/end) | none | no published quota. Server sheds load with `503 ServerOverloaded` + `Retry-After: 1`. Be serial and polite | **works**, ~0.35 s | community-contributed DB that explicitly invites API use |
| lrcmux (`api.lrcmux.dev`) | yes | none | unknown | works (beets added it in 2026). Returned LRC for Queen | unclear who runs it or what upstreams it uses. Treat as secondary |
| **NetEase** (`music.163.com/api`) | yes (+ translation `tlyric`) | none | undocumented. Throttles bursts | **works** | unofficial API. Grey area |
| QQ Music | yes (QRC word-level) | none | undocumented | not probed. Needs signed requests | unofficial. Grey area |
| Musixmatch official | richsync on paid tiers | yes | free tier is a ~30% lyric preview, non-commercial | needs key. Developer site blocks fetches | licensed, paid |
| Musixmatch "desktop" token API (used by syncedlyrics) | yes | anonymous token | aggressive captchas | fragile | breaks ToS |
| **Genius** | no | free OAuth token for search | API: ~reasonable. Page scraping gets IP-blocked if heavy | API needs token (401 without) | API ToS forbid lyric scraping |
| lyrics.ovh | no | none | – | 404 for a top-100 song | proxy for scraped data |
| ChartLyrics | no | none | – | **shut down** (404, LyricFinder dropped it in 2026-02) | – |
| Happi.dev | no | yes | – | 502 | – |
| AZLyrics / Lyrics.com / Tekstowo | no | none | anti-bot, IP bans | page reachable | ToS forbid automated access |
| Spotify lyrics (internal, licensed from Musixmatch) | yes | user `sp_dc` cookie | – | – | **forbidden** by Spotify Developer Terms |
| Apple Music | yes (TTML, in-app only) | – | – | no lyrics in MusicKit or the Apple Music API | **not available** |
| MusicBrainz | – (metadata only) | none, but needs a meaningful User-Agent | 1 req/s | works | CC0 core data |
| AcoustID | – (fingerprint → MB recording IDs) | free app key | 3 req/s | works | free for non-commercial use only |

## 3. Per-source notes and API shapes

### 3.1 LRCLIB — https://lrclib.net/docs, source https://github.com/tranxuanthang/lrclib

The server is written in Rust (Axum, SQLite and a Tantivy search index). Routes
in `server/src/router.rs` on `main` (repo pushed 2026-08):

- `GET /api/get?track_name=&artist_name=&album_name=&duration=` returns the
  single best record. Matching is exact on normalised (lower-cased) names, and
  duration must be within **±2 s** (`track_repository.rs`: `duration >= d-2 AND
  duration <= d+2`). It prefers records that have synced lyrics. Album and
  duration are optional in the current code. Both hits and misses are cached
  server-side for 24 h. If no record matches, it returns 404 `TrackNotFound`.
  Under load it returns `503 {"name":"ServerOverloaded"}` (one probe got this).
- `GET /api/get/{id}` fetches by id.
- `GET /api/search?q=…` or `?track_name=&artist_name=&album_name=` returns up to
  **20** records ranked by full-text search. Use `q` for messy input. A `q`
  search returns noisy user uploads (e.g. artist `"Bohemian Rhapsody - Queen"`,
  or track `"Queen - Bohemian Rapshody"`), so ranking on our side is essential.
- `/api/get-cached` from older docs **no longer exists** (404 now). `/get` no
  longer calls external providers either. It is a pure DB lookup.
- `POST /api/request-challenge` + `POST /api/publish` let clients contribute
  lyrics, using a proof-of-work token. beatmapper could later publish
  user-corrected LRC.
- Clients should send a descriptive `User-Agent`, e.g. `beatmapper/0.x
  (https://…)`, as the docs ask.
- A downstream client maintainer (Lyricade) reported that LRCLIB recently
  started rate-limiting and does not allow parallel requests (via
  https://flathub.org/kab/apps/io.github.weiteck.Lyricade). So send requests
  one at a time and honour `Retry-After`.
- SQLite dumps of the whole DB are listed at https://lrclib.net/db-dumps. An
  offline mirror is possible but is several GB.

Live response from `/api/search?track_name=Bohemian Rhapsody&artist_name=Queen`
(20 items, one shown, strings cut):

```json
{
  "id": 36902193,
  "name": "Bohemian rhapsody",
  "trackName": "Bohemian rhapsody",
  "artistName": "Queen",
  "albumName": "The greatest hits",
  "duration": 351.0,
  "instrumental": false,
  "hasWordSync": false,
  "plainLyrics": "Is this the real life? Is this just fantasy?\n…",
  "syncedLyrics": "[00:00.15] Is this the real life? Is this just fantasy?\n[00:…",
  "lyricsfile": "version: '1.0'\nmetadata:\n  title: Bohemian rhapsody\n  artist: Queen\n  album: …\n  duration_ms: 351000\n  instrumental: false\nlines:\n- text: Is this the real life? …\n  start_ms: 150\n  end_ms: 7130\n…"
}
```

The `lyricsfile` (YAML) and `hasWordSync` fields are new. `lyricsfile` gives
`start_ms`/`end_ms` per line, and per word when `hasWordSync` is true (LRCGET
2.2 added a word-sync editor). That is directly useful for lyric mapping,
because line *end* times are explicit. `/api/get` with album "A Night at the
Opera" and duration 354 returned id 19080 (duration 355.0), same shape.
Instrumental tracks have `instrumental: true` and null lyrics.

### 3.2 lrcmux — https://api.lrcmux.dev

beets 2.x added it as a backend. `GET /get?artist=&title=&duration=&album=&format=lrc|txt&level=line|none[&sources=a,b]`
returns **raw text** (LRC or plain), not JSON. A live probe returned correct
LRC for Queen. There is no public documentation on who runs it or which
upstreams it aggregates, and `/sources` returns 404. It's fine as a
"one more opinion" source, but don't make it primary.

### 3.3 NetEase Cloud Music (unofficial)

- Search: `GET https://music.163.com/api/search/get?s=<title artist>&type=1&limit=10`
  (send a browser UA and `Referer: https://music.163.com`). The response is
  `{"code":200,"result":{"songs":[{"id":1868553,"name":"Bohemian Rhapsody","artists":[{"name":"Queen"}],"duration":355195,…}]}}`.
  Duration is in **ms**.
- Lyrics: `GET https://music.163.com/api/song/lyric?id=<id>&lv=1&tv=-1`
  returns `{"lrc":{"lyric":"[00:00.000] 作词 : Freddie Mercury\n[00:00.025] 作曲 : …\n…"},"tlyric":{…},"code":200,…}`.
  `tlyric` holds the Chinese translation when present. Add `kv=1`/`yv=1` for
  word-level "yrc" on some endpoints.
- The first lines are often credits (`作词` lyricist, `作曲` composer, `编曲`
  arranger, `制作人` producer). Strip them. Pure-instrumental tracks return
  `"纯音乐，请欣赏"` ("pure music, please enjoy").
- syncedlyrics and many players use the same endpoints. They're unofficial and
  could change or get geo-throttled.

### 3.4 QQ Music (unofficial)

Good for Mandopop. It returns QRC (encrypted word-level) or base64 LRC. Requests
need signing, and the protocol changes often. Not worth it unless users ask for
Asian catalogue beyond NetEase.

### 3.5 Musixmatch — https://developer.musixmatch.com

The official API (`track.search`, `matcher.lyrics.get`, `track.subtitle.get`)
needs an API key. On the free plan, `lyrics_body` is a ~30% excerpt with a
tracking pixel and a non-commercial clause. Full text and subtitles (synced)
are paid, commercially licensed tiers. Third-party summaries only; the
developer site blocked fetching. syncedlyrics instead uses the Musixmatch
*desktop app* API (`apic-desktop.musixmatch.com`, anonymous `token.get`, then
`macro.subtitles.get`). That hits captchas quickly and breaks ToS. beets
excludes Musixmatch by default because it blocks the beets User-Agent. **Not
recommended.**

### 3.6 Genius — https://docs.genius.com

- `GET https://api.genius.com/search?q=…` with `Authorization: Bearer <token>`
  (token from a free API client). Without it, you get
  `{"meta":{"status":401,"message":"This call requires an access_token…"}}`.
  Hits give `result.title`, `result.primary_artist.name`, `result.url`, and
  `result.id`, but **no lyrics**. Genius licenses its lyrics and keeps them out
  of the API.
- Text comes from scraping `result.url`. beets uses a regex on the embedded
  JSON (`\"html\":\"…\"`). lyricsgenius (3.15.0, https://lyricsgenius.readthedocs.io)
  uses BeautifulSoup on `div[data-lyrics-container=true]`. Section tags like
  `[Chorus]` and "N Contributors…Lyrics" headers have to be stripped.
- The lyricsgenius docs say outright that this scraping breaks Genius's ToS,
  and heavy use gets IPs blocked.

### 3.7 lyrics.ovh, ChartLyrics, Happi

- `GET https://api.lyrics.ovh/v1/<artist>/<title>` returned
  `{"error":"No lyrics found"}` (404) for Queen/Bohemian Rhapsody. It has been
  unreliable since about 2021.
- `api.chartlyrics.com` returns nginx 404. The LyricFinder Perl module dropped
  it in 2026-02 because the service shut down (https://metacpan.org/dist/LyricFinder/changes).
- `api.happi.dev` returned 502.

### 3.8 Spotify and Apple Music

- Spotify's in-app lyrics (`spclient…/color-lyrics/v2/track/<id>`, licensed
  from Musixmatch) need a logged-in user's `sp_dc` cookie. The Spotify Developer
  Terms forbid using non-public endpoints and Spotify content outside
  Spotify-approved uses. **Do not use.**
- The Apple Music API and MusicKit do not expose lyrics, which are TTML inside
  the app only. **Not available.**

### 3.9 MusicBrainz (normalising)

`GET https://musicbrainz.org/ws/2/recording?query=recording:"Bohemian Rhapsody" AND artist:queen&fmt=json&limit=5`
returns `{"count":225,"recordings":[{"id":"01d2788d-…","score":100,"title":"Bohemian Rhapsody","length":139000,"artist-credit":[{"name":"Queen"}],"first-release-date":"2004",…}]}`.
It has many recordings (live versions, edits), so filter by length against the
file's duration. The limit is **1 req/s** and a meaningful User-Agent with
contact info is required (https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting).
Use it mainly to turn a fingerprint's recording ID into a canonical
title/artist, or to fix spelling before querying LRCLIB.

## 4. Existing tools, for reference

- **beets lyrics plugin** (https://beets.readthedocs.io/en/latest/plugins/lyrics.html,
  source `beetsplug/lyrics.py`):
  - Default `sources: lrclib google genius lrcmux`. Tekstowo is opt-in.
    Musixmatch is excluded (it blocks the beets UA).
  - Google Custom Search needs a key and allows 100 queries/day.
  - Heuristics worth copying:
    - `search_pairs()`: try the artist, then the artist with
      `feat./ft./featuring …` removed, then `artist_sort`. Skip "Various
      Artists". Try the title, then the title with the trailing `(…)` removed
      (live/remix/remaster), the title with `feat …` removed, and the title
      with everything after `:` removed. Split `A / B` into separate titles.
    - LRCLIB candidates: reject when `|cand.duration − track| > 5%`, or when
      the last LRC timestamp is beyond the track length. Pick the best by
      `(not synced, duration_dist)`.
    - Scraped sources: accept when `max(string_dist(artist), string_dist(title)) ≤ dist_thresh`
      (0.11, a normalised Levenshtein over unidecoded, lower-cased strings).
      Google results down-weight azlyrics (×0.5) and songlyrics (×0.6).
    - HTML cleanup via regexes: `<br>`→`\n`, `</p><p>`→blank line, remove
      `<script>`, comments, `<aside>`, empty divs and inline tags, then
      `remove_ads`.
    - `synced: yes` prefers LRC and never replaces existing synced lyrics with
      plain ones.
- **syncedlyrics** (https://github.com/moehmeni/syncedlyrics, MIT, last release
  2024-07): providers are Musixmatch, Lrclib, NetEase, Megalobiz, and Genius
  (plain). Deezer and Lyricsify are broken. It supports `--enhanced` for
  word-level LRC. The fork `syncedlyrics_aio` adds Tencent (QQ) and passes a
  duration hint to NetEase/QQ (https://pypi.org/project/syncedlyrics_aio/1.0.1).
- **lyricsgenius** 3.15.0: Genius API + scraper with `remove_section_headers`
  and `skip_non_songs` options.
- **LRCGET** (https://github.com/tranxuanthang/lrcget): the official LRCLIB
  desktop client (Tauri). Its matching is LRCLIB `/get` with the file's tags +
  duration, falling back to search.
- **MusicBrainz Picard**: tags via AcoustID. Lyrics plugins exist but mostly
  scrape. **Kodi** "culrc" / **foobar2000** ESLyric / OpenLyrics: OpenLyrics
  (foobar) ships providers for LRCLIB, NetEase, QQ Music, Musixmatch, Genius,
  AZLyrics, and others. This confirms the same practical provider set.

## 5. Identification pipeline

Run these steps in order and stop once the confidence is high enough.

1. **Use known metadata.** beatmapper's project, or the user's typed title and
   artist, overrides everything else.
2. **Read the file's tags.** Use `mutagen` in the sidecar (or TagLib/ffprobe).
   Read title, artist, album, and length, plus any embedded lyrics
   (`USLT`/`SYLT` in ID3, `©lyr` in MP4, `LYRICS` in Vorbis comments). Embedded
   lyrics become candidate #0 with source `embedded`.
3. **Parse the filename** when tags are missing. Strip the extension and any
   leading track number (`^\d{1,3}[\s._-]+`). Split on ` - ` / ` – ` / `_-_`
   into `Artist - Title` (also try `Title - Artist`, since LRCLIB `q` search
   ranks both). Remove noise like `(Official Video)`, `[HQ]`, `(Lyrics)`,
   `(Audio)`, `(Remastered 2011)`, `ft. X`, and YouTube IDs `[A-Za-z0-9_-]{11}`.
   Turn `_` into spaces.
4. **Fingerprint** when there's still no title/artist, or as a cross-check on
   request:
   - `fpcalc -json -length 120 file` (Chromaprint, `brew install chromaprint`;
     not installed on this machine yet) gives `{"duration":…, "fingerprint":"AQAD…"}`.
   - `GET https://api.acoustid.org/v2/lookup?client=<APPKEY>&duration=<int>&fingerprint=…&meta=recordings+releasegroups+compress`
     returns `results[].score` (0–1) and `results[].recordings[]{id,title,duration,artists[]}`.
     Take recordings from results with score > 0.8 and choose by duration
     closeness. (https://acoustid.org/webservice: free key, ≤3 req/s,
     non-commercial only.)
   - `pyacoustid` wraps both steps (`acoustid.match(key, path)`).
   - This also catches mislabeled files. If the fingerprint's title disagrees
     with the tags, show both.
5. **Normalise** (optional). Look up the MusicBrainz recording to get the
   canonical title spelling and artist credit, then query lyrics with both the
   raw and canonical strings.
6. **Duration.** Always pass the decoded audio length that beatmapper already
   knows. It is the strongest disambiguator (beets ignores its string distance
   for LRCLIB and uses duration alone).

## 6. Candidate generation, ranking, dedupe, cleanup

**Querying (serial, with stop-early):**

1. LRCLIB `/api/get` with (title, artist, album, duration). An exact hit with
   synced lyrics is very likely correct.
2. LRCLIB `/api/search` with track_name + artist_name. Then search with `q =
   "artist title"` for each beets-style title/artist alternative, stopping once
   ≥3 good candidates are found.
3. NetEase search + lyric for the top 3 songs within ±3 s.
4. lrcmux `/get` (optional).
5. Genius (optional, plain only, 1 search + 1 page).

Use a total timeout of about 10 s and a per-request timeout of 5 s. Cache
results on disk under `~/.cache/beatmapper/lyrics/<sha1(norm artist|title|dur)>.json`
for a few days, so reopening a track costs nothing. This mirrors the stems
cache.

**Score each candidate** (0–1, higher is better):

```
title_sim    = token_set_ratio(norm(q.title),  norm(c.title))      # rapidfuzz, 0..1
artist_sim   = token_set_ratio(norm(q.artist), norm(c.artist))     # 1.0 if query artist unknown
dur_score    = 1.0 if |Δ|<=2s, linear → 0 at |Δ| = max(10s, 5% of len); unknown → 0.5
sync_bonus   = 0.15 synced line-level, +0.05 word-level, 0 plain
lrc_fit      = 0 if last LRC timestamp > track length + 2s (reject), else 1
agreement    = mean text similarity to other sources' candidates (see dedupe), 0..1
source_prior = lrclib 1.0, netease .9, lrcmux .85, genius .8, embedded 1.0
score = lrc_fit * source_prior * (0.30*title_sim + 0.20*artist_sim + 0.25*dur_score
                                  + 0.15*agreement + sync_bonus)
```

`norm()` does NFKC, casefold, unidecode for Latin scripts, strips
`feat./ft.` clauses, trailing `(…)` / `[…]`, `- remaster…`, and punctuation,
and collapses whitespace. Drop candidates with `title_sim < 0.6` unless the
query came from a fingerprint, and drop `instrumental` ones unless nothing
else exists.

**Dedupe:** LRCLIB often has 10–20 near-identical uploads of one song. For each
candidate, compute a normalised plain body (cleaned lines, lower-cased, letters
only) and a 64-bit simhash, or simply `ratio()` on the first ~400 chars.
Cluster those with similarity ≥ 0.92. In each cluster keep the best-scoring
entry, preferring synced, then word-synced, then the closest duration. Record
`dupes=N` and the sources in the cluster. Clusters spanning several sources
earn the `agreement` bonus, since independent sources agreeing means the text
is probably right. Show the user at most about 6 clusters.

**Cleanup** (apply to plain text, and to the text part of each LRC line):

- Normalise line endings and Unicode (NFC). Turn `’` to `'` only for matching,
  never for display.
- Remove section/annotation tags: `^\s*[\[(](verse|chorus|pre-chorus|bridge|hook|intro|outro|refrain|instrumental|interlude|guitar solo|repeat.*|x\d+)[^\])]*[\])]\s*$`
  (case-insensitive). Keep a `--keep-sections` option, since section names may
  later seed beatmapper's section map.
- Remove Genius headers and footers: `^\d+ Contributors?.*Lyrics`, `…Lyrics$`
  in the first line, `\d*Embed$` at the end, "You might also like", and
  "See … Live" ads. For scraped sites, remove lines with `www.`, `lyrics
  provided by`, `submit corrections`, or copyright boilerplate.
- Remove NetEase credit lines (`作词|作曲|编曲|制作人|混音|母带|和声|吉他|贝斯|鼓 :`)
  and LRC header tags (`[ar:]`, `[ti:]`, `[al:]`, `[by:]`, `[offset:]`,
  `[length:]`). Apply `[offset:]` to the times before dropping it.
- Merge LRC lines that are empty or only `♪` into gaps. Collapse runs of more
  than one blank line. Trim trailing spaces. Keep one blank line between
  stanzas, since it marks a useful section boundary for the Lyric Index.
- Parse enhanced LRC (`<mm:ss.xx>` word tags) and LRCLIB `lyricsfile` YAML into
  (start_ms, end_ms, text[, words]) so the app gets a single format.

## 7. Sidecar sketch: `scripts/lyrics_find.py`

The script would run with the lyrics venv's Python (`external/venv-lyrics/bin/python`).
Its CLI:

```
lyrics_find.py [AUDIO_FILE] [--title T] [--artist A] [--album AL]
               [--duration SECS] [--sources lrclib,netease,lrcmux,genius]
               [--fingerprint] [--acoustid-key K] [--genius-token T]
               [--max N] [--no-cache] [--check]
```

- Tags are read from `AUDIO_FILE` when it's given. `--title/--artist/--duration`
  override them.
- `--fingerprint` runs `fpcalc` + AcoustID before the search.
- `--check` answers only from the cache (exit 3 if nothing is cached), the same
  as `stems_demucs.py`.

**Output.** Stdout gets one record per line, tab-separated. Lyric bodies hold
newlines (and maybe tabs), so they're written to files in the cache dir and the
line carries the path. This is the same idea as `stem<TAB>name<TAB>/path`.

```
status<TAB>reading tags
query<TAB>tags<TAB>Queen<TAB>Bohemian Rhapsody<TAB>A Night at the Opera<TAB>354.6
status<TAB>fingerprinting
query<TAB>acoustid<TAB>Queen<TAB>Bohemian Rhapsody<TAB><TAB>354.0<TAB>0.97
status<TAB>searching lrclib
progress<TAB>0.40
cand<TAB>1<TAB>0.93<TAB>lrclib:19080<TAB>synced<TAB>Queen<TAB>Bohemian Rhapsody<TAB>A Night at the Opera<TAB>355.0<TAB>63<TAB>14<TAB>~/.cache/beatmapper/lyrics/<h>/1.lrc<TAB>~/.cache/beatmapper/lyrics/<h>/1.txt<TAB>lrclib,netease
cand<TAB>2<TAB>0.71<TAB>netease:1868553<TAB>synced<TAB>Queen<TAB>Bohemian Rhapsody<TAB><TAB>355.2<TAB>62<TAB>1<TAB>…/2.lrc<TAB>…/2.txt<TAB>netease
cand<TAB>3<TAB>0.55<TAB>genius:1063<TAB>plain<TAB>Queen<TAB>Bohemian Rhapsody<TAB><TAB><TAB>58<TAB>1<TAB><TAB>…/3.txt<TAB>genius
warn<TAB>netease<TAB>HTTP 503, skipped
done<TAB>3
```

`cand` fields: rank, score, source id, kind (`word` | `synced` | `plain` |
`instrumental`), artist, title, album, duration (s, empty if unknown), line
count, dupes clustered, LRC path (empty if plain), plain-text path (always,
already cleaned), sources agreeing.

Exit codes: 0 means candidates were found, 2 means no candidates, 3 means not
cached (`--check`), 1 means failure. Each `cand` line is printed as soon as
it's ranked enough to show (stream LRCLIB results first and re-emit ranks at
the end with `rerank<TAB>id<TAB>rank<TAB>score`), or else all at the end. The
end-only version is simpler, and lookups take about 2 s anyway.

UI: list the candidates showing score, source, kind, duration delta and the
first two lines, with a preview pane. "Use" loads the `.txt` into the Lyric
Index the same way as clipboard paste. If an `.lrc` exists, it also offers its
timestamps as initial line times.

Native alternative: the C++ side calls LRCLIB `/api/search` and `/api/get` via
libcurl (already present on macOS) and parses JSON with any single-header
library. That covers the main case without Python. NetEase and Genius stay in
the sidecar.

## 8. Legal and ToS notes

- **Copyright.** Lyrics are copyrighted text owned by publishers, whatever the
  source. Fetching them for a user's personal practice/mapping session and
  storing them locally is the same thing LRCGET, beets, foobar and Kodi
  plug-ins do. **Redistributing** them is different: bundling lyrics in shared
  project files, exporting to others, or a server-side cache needs a licence
  (Musixmatch, LyricFind). So keep fetched text in the user's local cache and
  project only, and show the source.
- **LRCLIB** is built for free programmatic use. It asks for a proper
  User-Agent and fair use (be serial, honour `503` + `Retry-After`).
  Contributions are user-submitted. It makes no licensing promise about the
  lyrics themselves.
- **NetEase/QQ.** The endpoints are unofficial and undocumented, with no
  explicit permission. The risk is mainly breakage or blocking, but it is a ToS
  grey area.
- **Genius.** The API Terms allow the API for metadata. Scraping lyric pages is
  against Genius's ToS, as lyricsgenius's docs say. Keep it opt-in and low
  volume, or leave it out.
- **AZLyrics, Lyrics.com, Tekstowo, Megalobiz.** Their ToS forbid automated
  access, and AZLyrics bans IPs that scrape. robots.txt only disallows
  `/search/` and `/lyricsdb/`, but the ToS still apply. Not recommended.
- **Musixmatch.** The free API is a non-commercial preview. The desktop-token
  API and Spotify's internal endpoint break their ToS. **Avoid.**
- **AcoustID** is free for non-commercial use only. If beatmapper is ever sold,
  it needs a commercial AcoustID plan (acoustid.biz). **MusicBrainz** data is
  CC0 but rate-limited to 1 req/s.

## Sources

- LRCLIB: https://lrclib.net/docs, https://github.com/tranxuanthang/lrclib (`server/src/router.rs`, `routes/*.rs`, `repositories/track_repository.rs`, `ARCHITECTURE.md`), https://lrclib.net/db-dumps
- LRCGET: https://github.com/tranxuanthang/lrcget, https://newreleases.io/project/github/tranxuanthang/lrcget/release/2.2.0
- Lyricade (LRCLIB rate-limiting report): https://flathub.org/kab/apps/io.github.weiteck.Lyricade
- beets lyrics plugin: https://beets.readthedocs.io/en/latest/plugins/lyrics.html, https://github.com/beetbox/beets/blob/master/beetsplug/lyrics.py
- syncedlyrics: https://github.com/moehmeni/syncedlyrics, https://pypi.org/project/syncedlyrics/, https://pypi.org/project/syncedlyrics_aio/1.0.1
- lyricsgenius: https://lyricsgenius.readthedocs.io/en/latest/how_it_works.html; Genius API: https://docs.genius.com
- Musixmatch: https://developer.musixmatch.com, https://freeapihub.com/apis/musixmatch, https://code.thetadev.de/ThetaDev/musixmatch-inofficial
- ChartLyrics shutdown: https://metacpan.org/dist/LyricFinder/changes
- lyrics.ovh: https://community.thunkable.com/t/about-lyrics-ovh-api/2019282
- AcoustID: https://acoustid.org/webservice, Chromaprint: https://acoustid.org/chromaprint, pyacoustid: https://github.com/beetbox/pyacoustid
- MusicBrainz: https://musicbrainz.org/doc/MusicBrainz_API, https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting
- Spotify Developer Terms: https://developer.spotify.com/terms
- Live probes (2026-10-11): lrclib.net `/api/search`, `/api/get`, `/api/get-cached` (404); api.lrcmux.dev `/get`; music.163.com `/api/search/get` and `/api/song/lyric`; musicbrainz.org `/ws/2/recording`; api.genius.com `/search` (401); api.lyrics.ovh (404); api.chartlyrics.com (404); api.happi.dev (502).
