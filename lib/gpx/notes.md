# Guitar Pro Parser in Virgil — Research Notes

## Sources

- TuxGuitar Java implementation: `~/Code/tuxguitar/common/TuxGuitar-gtp/src/app/tuxguitar/io/gtp/`
- Virgil layouts tutorial: `~/Code/virgil/doc/tutorial/Layouts.md`
- Virgil DataReader: `~/Code/virgil/lib/util/DataReader.v3`
- Existing RiffHound IR: `~/RiffHound/RiffHound/rifftext/RiffIr.v3`
- Test GP files: `~/RiffHound/tabs/` (92 files: GP3, GP4, GP5)

---

## Guitar Pro File Format Overview

Guitar Pro files are little-endian binary files. There are three relevant
versions: GP3, GP4, GP5 (also GP1/GP2 which TuxGuitar supports but we can skip).

### Version Detection (all versions)

The file begins with a 31-byte header:
- byte 0:    length of version string (usually 24)
- bytes 1-30: version string, padded with null bytes to 30 chars

Known version strings:
| String                       | Version |
|------------------------------|---------|
| `FICHIER GUITAR PRO v3.00`   | GP3     |
| `FICHIER GUITAR PRO v4.00`   | GP4 v0  |
| `FICHIER GUITAR PRO v4.06`   | GP4 v1  |
| `FICHIER GUITAR PRO L4.06`   | GP4 v2  |
| `FICHIER GUITAR PRO v5.00`   | GP5 v0  |
| `FICHIER GUITAR PRO v5.10`   | GP5 v1  |

### GP3 File Structure (sequential)

```
[version header]          31 bytes
[song info]               variable (8 × IntSizedString + N comment lines)
[triplet_feel]            1 byte bool
[tempo]                   4 bytes (i32, BPM)
[key_signature]           1 byte (signed: 0=C, 1=G, -1=F, etc.)
[skip]                    3 bytes
[channels]                64 × 12 bytes
[measure_count]           4 bytes (i32)
[track_count]             4 bytes (i32)
[measure_headers]         variable (measure_count × header)
[tracks]                  variable (track_count × track)
[measures]                variable (measures × tracks × measure)
```

### GP4 Additions over GP3

After song info, before tempo:
```
[lyric_track]             4 bytes (i32, 1-indexed track)
[lyrics]                  4+IntString + 4×(4+IntString)  = 5 lyric lines
```
After key signature:
```
[octave]                  1 byte (skip)
```
In note effects: 2-byte flags (was 1), adds tremolo picking, slide type byte,
harmonics by type byte, trills (fret + period).
In beat effects: 2-byte flags (was 1), adds pick strokes.
In mix change: extra byte at end.
Track has lyric reference.

### GP5 Additions over GP4

After lyrics, before tempo:
```
[page_setup]              variable (30 or 49 bytes + 11 × (4 + ByteSizedString))
```
Version 5.10 (versionCode > 0) extras:
- skip(1) after tempo
- track: skip(49 instead of 44) + 2 extra strings
- readTracks: skip(1 instead of 2) at end
- mix change: extra tempo skip + 2 extra strings

Structural differences:
- 2 voices per beat (loops voice 0 then voice 1)
- Beat: getBeat(start) — finds or creates beat by start time; extra skip(1) and read+maybe skip(1) at end
- Note: extra flags (0x02 heavy accent, 0x01 skip(8)); skip(1) always before effects; effects take note fret value
- Measure header: different repeat alternative/triplet feel layout; conditional skip(4) for time sig
- readTracks: trailing skip at end
- Chord: always uses the 74-byte "new" format

### String Encoding

Strings come in several flavors:
- `IntSizedString`: 4-byte length + that many bytes
  (readStringInteger)
- `ByteSizedString(N)`: 1-byte length + N bytes total (length bytes + padding to N)
  (readStringByte)
- `ByteSizeSelf`: (len+1)-byte length byte + len bytes
  (readStringByteSizeOfByte)
- `ByteSizeOfInteger`: 4-byte prefix gives block size, 1-byte length + content bytes
  (readStringByteSizeOfInteger)

Most song metadata strings use `ByteSizeOfInteger`.

### Channel Entry (12 bytes)

64 entries, always present:
```
program:  i32   (MIDI program number)
volume:   i8
balance:  i8
chorus:   i8
reverb:   i8
phaser:   i8
tremolo:  i8
_pad:     u8
_pad:     u8
```
Channel 9 (0-indexed) is the percussion channel.

### Track Entry

```
[flags]            1 byte
[name]             StringByte(40)  — 1-byte len + 40 bytes total
[string_count]     i32
[string_tuning]    7 × i32  (MIDI note value; only first string_count used)
[midi_port]        i32  (skip)
[gm_channel1]      i32  (1-indexed → 0-indexed)
[gm_channel2]      i32  (1-indexed → 0-indexed)
[frets]            i32  (skip)
[offset]           i32
[color]            4 bytes (R G B pad)
```
GP5: extra skips and version-conditional extra strings.

### Measure Header

```
[flags]            1 byte
  0x01: new time signature numerator → read byte
  0x02: new time signature denominator → read byte
  0x04: repeat_open
  0x08: repeat_close → read byte (GP5: (byte & 0xff) - 1)
  0x10: repeat_alternative → read byte (GP5: direct; GP3/4: parsed)
  0x20: has_marker → read marker (string + color)
  0x40: key_change → read key (byte + skip(1))
GP5 additional:
  0x01|0x02: skip(4)  (extra time sig data)
  triplet_feel byte always present at end (GP5 only)
GP5: skip(1) between measure headers (after first)
```

### Beat

```
[flags]           1 byte
  0x40: beat_type → read byte (GP5: sets voice empty)
[duration_value]  i8  (power: value = 2^(x+4)/4; -2=whole,−1=half,0=quarter,1=eighth,2=16th,3=32nd)
[flags & 0x20]: tuplet → read i32 (3,5,6,7,9,10,11,12; GP5 also 13)
[flags & 0x01]: dotted
[flags & 0x02]: chord → read chord (skip or frets)
[flags & 0x04]: text → read IntString
[flags & 0x08]: beat effects → read beat effects
[flags & 0x10]: mix change → read mix change
[string_flags]  1 byte (bits 6..0 for strings 0..6, high bit = string 1)
  → for each active string: read note
GP5: skip(1); read byte; if (byte & 0x08) skip(1)
```

### Note

GP3:
```
[flags]          1 byte
  0x04: ghost note
  0x20: note_type byte  (1=normal, 2=tied, 3=dead)
  0x01: skip(2)  (duration/tuplet)
  0x10: velocity byte
  0x20: fret byte
  0x80: skip(2)  (fingering)
  0x08: note effects
```
GP4 adds: 0x40 = accentuated; note effects get 2-flag bytes.
GP5 adds: 0x02 = heavy accent; no skip(2) for 0x01 (instead skip(8)); always skip(1) before effects.

### Bend

```
skip(5)          (type, value placeholder)
[num_points]     i32
for each point:
  position       i32  (0..60, maps to 0..MAX_POSITION_LENGTH)
  value          i32  (semitones × 25, maps to ±SEMITONE_LENGTH)
  vibrato        i8   (skip)
```

### Grace Note

GP3/4:
```
fret       u8  (255 = dead)
dynamic    u8  (velocity level)
transition u8  (0=none,1=slide,2=bend,3=hammer)
duration   u8
```
GP5:
```
fret       u8
dynamic    u8
transition i8
duration   u8
flags      u8  (0x01=dead, 0x02=on_beat)
```

---

## Virgil Layouts Strategy

The GP format is mostly sequential with variable-length sections, so we
read it from a flat byte array using `DataReader`. Layouts are used for
the fixed-structure repetitions.

### Natural uses of layouts

**Channel entry** (12 bytes × 64 entries):
```virgil
layout GpxChannelEntry {
    +0   program:  i32;
    +4   volume:   i8;
    +5   balance:  i8;
    +6   chorus:   i8;
    +7   reverb:   i8;
    +8   phaser:   i8;
    +9   tremolo:  i8;
    =12;
}
```

**Color** (4 bytes):
```virgil
layout GpxColor {
    +0   r:  u8;
    +1   g:  u8;
    +2   b:  u8;
    =4;
}
```

**String tuning** (4 bytes × 7 per track):
```virgil
layout GpxStringTuning {
    +0   value: i32;
    =4;
}
```

**Version header** (31 bytes):
```virgil
layout GpxVersionHeader {
    +0   len:   u8;
    +1   str:   u8[30];
    =31;
}
```

For the 64-channel block, a `Ref<GpxChannelEntry>.at(data, base + i * 12)` lets
us read each channel's fields without byte-by-byte extraction.

The `DataReader` from `lib/util` handles the variable-length sequential reading
(read1, read_u32, readN, skipN). We layer layout reads on top for fixed structures.

**Key pattern:**
```virgil
var r = DataReader.new(fileData);
// ... sequential reads ...
// For channel block:
var chbase = r.pos;
r.skipN(64 * 12);
for i < 64 {
    var ch = Ref<GpxChannelEntry>.at(fileData, chbase + i * 12);
    // access ch.program, ch.volume, etc.
}
```

---

## Final IR Design

Decisions:
- `GpxNoteEffect` and `GpxBeatEffect`: separate flat classes (faithful to file sections)
- Duration: enum (raw i8 from file) + dotted bool + tuplet u8 (raw divisor, 0=none)
- Strings: 1-indexed (as in file)
- Two voice arrays on GpxMeasure (`voice0`/`voice1`; voice1 null for GP3/4)
- Arrays (not Vectors) for beats/notes since counts are known upfront
- Full effect fidelity for round-trip

### `GpxIR.v3`

```virgil
// Guitar Pro file format variant.
enum GpxFormat { GP3, GP4, GP5 }

// Note duration value. raw: -2=whole, -1=half, 0=quarter, 1=8th, 2=16th, 3=32nd, 4=64th
// Formula: 2^(raw+4)/4 quarter-note units
enum GpxNoteValue(raw: i8) {
    WHOLE(-2), HALF(-1), QUARTER(0), EIGHTH(1), SIXTEENTH(2), THIRTY_SECOND(3), SIXTY_FOURTH(4)
}

// Beat duration: note value + optional dotted + optional tuplet divisor.
// tuplet: 0=none; 3,5,6,7,9,10,11,12 (GP5 also 13) = divisor of the tuplet read from file
type GpxDuration(value: GpxNoteValue, dotted: bool, tuplet: u8) {}

// Bend/tremolo-bar control point. position: raw 0..60 from file; value: raw semitone×25.
type GpxBendPoint(position: int, value: int) {}

// A bend or tremolo bar envelope.
class GpxBend {
    var points: Array<GpxBendPoint>;
    new(points) {}
}

enum GpxHarmonicType { NONE, NATURAL, ARTIFICIAL, TAPPED, PINCH, SEMI }
enum GpxGraceTransition { NONE, SLIDE, BEND, HAMMER }

// A grace note. {dead}/{onBeat} flags from GP5 flags byte; inferred for GP3/4.
class GpxGrace {
    var fret: u8;
    var dynamic: u8;
    var transition: GpxGraceTransition;
    var duration: u8;
    var dead: bool;
    var onBeat: bool;
}

// Note-level effects section. null = section absent from file.
class GpxNoteEffect {
    var bend: GpxBend;          // null = no bend
    var grace: GpxGrace;        // null = no grace note
    var harmonicType: GpxHarmonicType;
    var harmonicData: u8;       // TAPPED: natural-freq index; ARTIFICIAL: 0
    var trillFret: i8;
    var trillPeriod: u8;        // 0=none; 1=16th, 2=32nd, 3=64th (raw from file)
    var tremoloPickRaw: u8;     // 0=none; 1=8th, 2=16th, 3=32nd (raw from file)
    var hammer: bool;
    var letRing: bool;
    var vibrato: bool;
    var slide: bool;
    var slideType: u8;          // GP4+ only: type byte following slide flag
    var palmMute: bool;
    var staccato: bool;
    var ghost: bool;
    var dead: bool;
    var accentuated: bool;
    var heavyAccent: bool;      // GP5 only
}

// Beat-level effects section. null = section absent from file.
class GpxBeatEffect {
    var fadeIn: bool;
    var vibrato: bool;
    var slapEffect: u8;         // 0=none, 1=tapping, 2=slapping, 3=popping
    var tremoloBar: GpxBend;    // null = none
    var strokeDown: u8;         // raw stroke byte (1–6), 0=none
    var strokeUp: u8;           // raw stroke byte (1–6), 0=none
    var pickStroke: u8;         // 0=none, 1=up, 2=down (GP4+ only, raw dir byte)
    // GP3 beat harmonics stored here (GP3 puts harmonics in beat effects, not note effects)
    var naturalHarmonic: bool;
    var artificialHarmonic: bool;
}

// A chord diagram.
class GpxChord {
    var name: string;
    var firstFret: int;
    var frets: Array<int>;
    var oldFormat: bool;        // GP3/4: true if read from the old (short) chord format
}

// A mid-measure mix change. -1 in any field = no change for that parameter.
class GpxMixChange {
    var instrument: i8;
    var volume: i8;
    var pan: i8;
    var chorus: i8;
    var reverb: i8;
    var phaser: i8;
    var tremolo: i8;
    var tempo: int;             // -1 = no change
    var tempoName: string;      // GP5 only, null otherwise
}

// A single note on a string.
class GpxNote {
    var string: u8;             // 1-indexed (as in file)
    var value: u8;              // fret number 0..99
    var tied: bool;
    var velocity: u8;
    var effect: GpxNoteEffect;  // null = no effects section
}

// One beat in a voice.
class GpxBeat {
    var start: long;            // tick position (computed during parse)
    var duration: GpxDuration;
    var notes: Array<GpxNote>;
    var empty: bool;            // GP5: explicitly marked as rest
    var text: string;           // null = none
    var chord: GpxChord;        // null = none
    var effect: GpxBeatEffect;  // null = no beat-effects section
    var mixChange: GpxMixChange;// null = none
}

// Per-track data for one measure. voice1 null for GP3/4 (single-voice).
class GpxMeasure {
    var voice0: Array<GpxBeat>;
    var voice1: Array<GpxBeat>;
}

// A rehearsal marker.
class GpxMarker {
    var measure: int;
    var title: string;
    var r: u8; var g: u8; var b: u8;
    new(measure, title, r, g, b) {}
}

// Global header for one measure across all tracks.
class GpxMeasureHeader {
    var number: int;
    var numerator: u8;
    var denominator: u8;        // raw denominator byte from file (e.g. 2=half, 4=quarter)
    var tempo: int;             // BPM at this measure start (from global/mix-change tracking)
    var repeatOpen: bool;
    var repeatClose: int;       // 0 = none
    var repeatAlternative: int; // bitmask
    var marker: GpxMarker;      // null = none
    var keySignature: int;      // 0=C, 1=G, -1=F, ... (converted from raw signed byte)
    var tripletFeel: u8;        // 0=none, 1=eighth, 2=sixteenth
}

// A guitar string with its open MIDI tuning value (1-indexed number as in file).
type GpxString(number: u8, value: int) {}

// A MIDI channel entry (one of the 64 in the file).
class GpxChannel {
    var program: int;
    var volume: i8;
    var balance: i8;
    var chorus: i8;
    var reverb: i8;
    var phaser: i8;
    var tremolo: i8;
    var percussion: bool;
}

// One lyric line (GP4+). Five stored per song (first is active, rest may be empty).
type GpxLyricLine(from: int, text: string) {}

// A track.
class GpxTrack {
    var number: int;
    var name: string;
    var strings: Array<GpxString>;      // up to 7, 1-indexed
    var channel1: u8;                   // 0-indexed into song.channels
    var channel2: u8;
    var offset: int;
    var color: (u8, u8, u8);
    var lyrics: Array<GpxLyricLine>;    // null for GP3; 5 entries for GP4+
    var measures: Array<GpxMeasure>;
}

// Top-level song.
class GpxSong {
    var format: GpxFormat;
    var versionCode: int;               // sub-version (0 or 1)
    var title: string;
    var artist: string;
    var album: string;
    var author: string;
    var copyright: string;
    var writer: string;
    var comments: string;
    var channels: Array<GpxChannel>;    // always 64 entries
    var headers: Array<GpxMeasureHeader>;
    var tracks: Array<GpxTrack>;
    var pageSetup: Array<byte>;         // GP5 only: raw page setup bytes; null otherwise
}
```

---

## Implementation Plan

### Files

```
lib/gpx/
  GpxLayouts.v3    — layout declarations for fixed binary structures
  GpxIR.v3         — IR class/type/enum declarations
  GpxParser.v3     — parser logic
  Main.v3          — entry point: load file, parse, print summary
```

### GpxLayouts.v3

Define layouts:
- `GpxVersionHeader` (31 bytes)
- `GpxChannelEntry` (12 bytes)
- `GpxColor` (4 bytes)
- `GpxStringTuning` (4 bytes)

### GpxParser.v3

Core structure:
```virgil
class GpxParser {
    var data: Array<byte>;
    var r: DataReader;
    var version: GpxVersion;
    var versionCode: int;     // sub-version (0 or 1)

    new(data) {
        this.r = DataReader.new(data);
    }

    def parse() -> GpxSong { ... }
    def readVersion() -> GpxVersion { ... }
    def readInfo(song: GpxSong) { ... }
    def readChannels() -> Array<GpxChannel> { ... }
    def readMeasureHeaders(count: int) -> Array<GpxMeasureHeader> { ... }
    def readTracks(...) -> Array<GpxTrack> { ... }
    def readMeasures(...) { ... }
    def readMeasure(...) -> GpxMeasure { ... }
    def readBeat(voice: int, ...) -> GpxBeat { ... }
    def readNote(...) -> GpxNote { ... }
    def readNoteEffects(...) -> GpxNoteEffect { ... }
    def readBend() -> GpxBend { ... }
    def readGrace() -> GpxGrace { ... }
    // ...helpers: readInt, readByte, readString*, skip
}
```

The parser will use `DataReader` for sequential reads and grab layout refs
for fixed-structure blocks (channels).

### Main.v3

```virgil
component Main {
    def main(args: Array<string>) -> int {
        if (args.length < 1) { System.puts("Usage: gpxparser <file>\n"); return 1; }
        var data = System.fileLoad(args[0]);
        if (data == null) { System.puts("Cannot open file\n"); return 1; }
        var song = GpxParser.new(data).parse();
        if (song == null) { System.puts("Parse error\n"); return 1; }
        // Print summary
        System.puts("Title: "); System.puts(song.title); System.ln();
        System.puts("Artist: "); System.puts(song.artist); System.ln();
        System.puts("Tracks: "); System.puti(song.tracks.length); System.ln();
        System.puts("Measures: "); System.puti(song.headers.length); System.ln();
        return 0;
    }
}
```

### Build command

```bash
v3i ~/RiffHound/RiffHound/lib/gpx/*.v3 \
    ~/Code/virgil/lib/util/DataReader.v3 \
    ~/Code/virgil/lib/util/Vector.v3 \
    ~/Code/virgil/lib/util/Arrays.v3 \
    ~/Code/virgil/lib/util/Strings.v3 \
    ~/Code/virgil/lib/util/StringBuilder.v3 \
    ~/RiffHound/tabs/BBKing-TheThrillIsGone.gp3
```

---

## Key Challenges

1. **String encoding**: GP files use a legacy charset (ISO-8859-1). Virgil strings
   are byte arrays. We can store them as-is and handle display separately.

2. **GP5 voice merging**: GP5 reads voice 0 and voice 1 separately (two separate
   loops over beats). Beats at the same start time are shared. The `getBeat(start)`
   logic finds an existing beat by start time or creates a new one.

3. **Tied note resolution**: A tied note has no fret value; we must look back through
   previous measures to find the last note on that string. This requires keeping
   track state during parsing. Simplest: scan backwards through already-parsed beats.

4. **Repeat alternative parsing (GP3/4)**: TuxGuitar converts the raw "value > i"
   logic to a bitmask based on existing alternatives. We should do the same.

5. **Mix change negative values**: GP uses signed bytes where -1 means "no change".
   We need to preserve the sign semantics.

6. **Version-specific conditionals**: Many branches on `versionCode > 0` (GP5.10)
   and `version == GP5`. These need careful handling.

---

## Notes on Layout Usage

The clearest wins for layouts are:

1. **Channel block**: 64 × 12 bytes. Instead of reading byte-by-byte in a loop,
   use `Ref<GpxChannelEntry>.at(data, r.pos + i * 12)` then `r.skipN(64 * 12)`.
   This is safe, readable, and efficient.

2. **Color**: `Ref<GpxColor>.at(data, r.pos)` for the 4-byte read + skip(4).

3. **String tunings**: 7 × 4 bytes per track; `Ref<GpxStringTuning>.at(data, pos)`.

4. **Version header**: `Ref<GpxVersionHeader>.of(data)` for the 31-byte prefix
   lets us access `.len` and `.str[]` cleanly.

Layouts are not beneficial for the variable-length portions (strings, beat data,
note effects) since those require sequential parsing with branches.
