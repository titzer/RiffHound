# karaoke (terminal)

The RiffHound karaoke screen in a terminal: lyrics with a hopping square,
the next four bars of chords in block letters, the chart's loops, and a load
dialog with the web client's search, filters and lists.

    make
    ./karaoke ~/playalong ~/backing_tracks     # these folders, remembered
    ./karaoke                                  # the remembered folders
    ./karaoke -t 92 ~/playalong/song.mp3       # one song, playing, from 0:92

Folders named on the command line are saved to
`~/.config/riffhound/karaoke.conf` (`dir PATH` lines; edit freely). The
audio engine's chatter goes to `~/.config/riffhound/karaoke.log`.

Audio is beatmapper's engine (`beatmapper/src/audio.cpp`, `stretch.cpp`),
compiled from its sources into `build/`. That is miniaudio output,
CoreAudio decoding for .m4a, and the stretch engine that will do speed and
pitch.

## Keys

| key | |
|---|---|
| space | play / stop (stop keeps the place) |
| ← → | back / forward 5 s (inside the current loop, when one is playing) |
| home | to the start (of the loop, when looping) |
| l or / | load dialog |
| 1–9 | add that chart loop to the loop programme (the first one starts it) |
| ⌫ | drop the last loop from the programme |
| o | clear the programme |
| - = \\ | speed down / up / reset (*stub*) |
| [ ] | transpose ±1 semitone (*stub*) |
| ; ' | fine tune ±1 cent (*stub*) |
| p | reset transpose and fine tune (*stub*) |
| q, ^C | quit |

Load dialog: type to search (subsequence match, ranked by alignment, then
recency). ↑↓ PgUp PgDn move, Enter plays, Tab switches between Tracks and
Lists, Esc backs out of a list, then clears the query, then closes. ^L ^D ^B
^O ^V ^T toggle the lyrics / chords / beats / loops / melody / trade filters.
The six flag columns show `l c b o ♪ ⇄` for what each chart carries.

## How bars are found

The web client measures chords in seconds against one median bar length,
which is why its grouping drifts. Here everything sits on the beat grid. Bars
are counted in beats from *anchors* known to be downbeats:

- `BM` beats (none of today's charts have them);
- section starts;
- the start of each `chords:` line, since the chord tool writes one name per bar.

A bar that runs into the next anchor is cut short there, so a pickup or a
section mark a beat late costs one short bar, not a shifted song. Blocks of
four bars restart at every section. The chord row pages by block and shows
the following block in small text underneath.

## Not yet

- **Server mode.** `--server URL` and `server` lines in the config are
  accepted, but the dialog only reports them as unsupported. The local scan
  follows `server.py`'s rules (extensions and preference order, `.riffignore`,
  hidden files, `list-<Name>.txt` resolution), so switching sources should not
  change what the dialog shows.
- **Speed and pitch.** The keys and readouts are final. `Player::apply_tuning`
  is the one place that will call `audio_set_speed` / `audio_set_pitch`.
- **Gapless loop programmes.** One loop wraps gaplessly in the engine. Moving
  between entries of a programme is a seek after the wrap is seen, which can
  hiccup for up to a frame. A "jump to" in the stretch source would fix it.

## Testing without a terminal

    ./karaoke --dump 30,31.5 --size 100x30 TRACK     # frames as plain text, no audio
    ./karaoke --dump-picker "bdog" DIR ...           # the load dialog
    ./karaoke --dump-picker "" --keys '\t\r' DIR     # keys first: \t \r \e \d \u, ^X
