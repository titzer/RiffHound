# MP3 Parser — Session 1 Summary

A complete MP3 file structure parser and metadata dumper in Virgil.

### Files

| File | Purpose |
|------|---------|
| `Mp3Layouts.v3` | Virgil `layout` definitions for fixed binary structures |
| `Mp3Ir.v3` | Intermediate representation: enums, types, classes |
| `Mp3Parser.v3` | Parser class: ID3v2/v1 tags + first MPEG frame detection |
| `mp3dumper.main.v3` | CLI dumper (mirrors `gpxdumper`) |
| `Makefile` | `make`, `make check`, `make run FILE=...`, `make clean` |

---

## IR Overview (`Mp3Ir.v3`)

```
Mp3File
  ├── id3v2: Mp3Id3v2Tag          (null if absent)
  │     ├── version: Mp3Id3Version  (V2_2 | V2_3 | V2_4)
  │     ├── flags, revision, tagSize
  │     └── frames: Array<Mp3Frame>
  │           ├── id: string         (3-char v2.2, 4-char v2.3/v2.4)
  │           ├── flags0, flags1: u8
  │           └── data: Range<byte>  (slice of file buffer)
  ├── audio: Mp3AudioData          (null if no MPEG frames found)
  │     ├── data: Range<byte>      ← RAW AUDIO BYTES (all frames to EOF)
  │     ├── offset: int            (byte offset of first frame in file)
  │     ├── firstFrame: Mp3AudioInfo
  │     │     ├── mpegVersion: Mp3MpegVersion  (MPEG1|MPEG2|MPEG25)
  │     │     ├── layer: Mp3Layer              (LAYER1|LAYER2|LAYER3)
  │     │     ├── bitrateKbps, sampleRateHz
  │     │     ├── channelMode: Mp3ChannelMode  (STEREO|JOINT_STEREO|DUAL_CHANNEL|MONO)
  │     │     ├── channels: int    (1 or 2)
  │     │     ├── padded, hasCrc: bool
  │     │     ├── frameSize: int   (total bytes incl. 4-byte header)
  │     │     └── samplesPerFrame: int  (384/576/1152)
  │     ├── xing: Mp3XingInfo      (null if no Xing/Info header)
  │     │     ├── isVbr: bool      (true=Xing VBR, false=Info CBR)
  │     │     ├── frameCount, streamSize, quality: int
  │     │     ├── hasToc: bool
  │     │     └── lameVersion: string  (null if no LAME tag)
  │     └── frameCount: int        (from Xing if present, else 0)
  └── id3v1: Mp3Id3v1              (null if absent)
        ├── title, artist, album, year, comment: string
        ├── track: int             (0 = v1.0, no track field)
        └── genre: int             (raw genre byte)
```

---

## Parser Details (`Mp3Parser.v3`)

### ID3v2 Tag Parsing
- Detects `"ID3"` magic at byte 0, reads synchsafe tag size from bytes 6–9.
- Handles all three major versions:
  - **v2.2**: 3-char frame IDs, 3-byte plain-BE frame sizes, no flag bytes.
  - **v2.3**: 4-char IDs, 4-byte plain-BE frame sizes (`u32`, NOT synchsafe).
  - **v2.4**: 4-char IDs, 4-byte **synchsafe** frame sizes. This distinction is
    the single most common ID3 parsing bug; always branch on version byte.
- Skips extended headers (flag bit 6) for both v2.3 and v2.4.
- Stores each frame's body as `Range<byte>` into the file buffer (zero-copy).
- Stops frame parsing on a zero byte (padding) or invalid frame ID.

### Synchsafe Integer Decoding
Each byte contributes only its low 7 bits (MSB always 0):
```
decoded = (b[0]&0x7F)<<21 | (b[1]&0x7F)<<14 | (b[2]&0x7F)<<7 | (b[3]&0x7F)
```
Used for: ID3v2 tag size, ID3v2.4 frame sizes, ID3v2.4 extended header size.

### MPEG Audio Frame Detection
- Scans forward from end of ID3v2 tag for `0xFF` followed by `(next & 0xE0) == 0xE0`.
- Validates the 4-byte header (version, layer, bitrate index, sample rate index).
- Confirms with a second sync word at the computed frame-size distance (CBR) or
  accepts at near-EOF.
- Decodes all fields from the 32-bit header (see layout in `Mp3Layouts.v3`).
- Frame size formula: Layer I → `(12*br*1000/sr + pad)*4`; Layer II/III → `144*br*1000/sr + pad`.

### Xing/Info VBR Header
Located inside the first audio frame's silent data, at a fixed offset past the
4-byte frame header and side-information block:
- MPEG-1 stereo: 4 + 32 = offset 36
- MPEG-1 mono:   4 + 17 = offset 21
- MPEG-2 stereo: 4 + 17 = offset 21
- MPEG-2 mono:   4 +  9 = offset 13

Tag is `"Xing"` (VBR) or `"Info"` (CBR). Followed by a 4-bit flags word, then
optional: frame count (4 bytes), stream size (4 bytes), 100-byte seek TOC,
quality indicator (4 bytes). LAME version string (`"LAME3.xxx"`) follows
immediately after.

### ID3v1 Tag
128 bytes at `file_size - 128`; detected by `"TAG"` magic. v1.1 extension:
byte at offset 125 = `0x00` → byte at offset 126 is track number.

---

## Dumper (`mp3dumper.main.v3`)

```
mp3dumper [options] <file.mp3> ...
  --info          Summary (default): ID3v2 version, common text fields, audio info
  -f, --frames    List all ID3v2 frames with sizes
  -t, --text      Decode text frame bodies (use with --frames)
  -a, --audio     Full MPEG frame header details + Xing info
  --id3v1         Show ID3v1 tag
```

Text frame decoding handles all four encodings:
- `0x00` ISO-8859-1 / `0x03` UTF-8: raw bytes (trim null terminator).
- `0x01` UTF-16 with BOM: detect LE (`FF FE`) vs BE (`FE FF`), extract ASCII
  code units (high byte = 0x00). **Do not pre-trim** — the trailing `0x00` is
  the high byte of the last ASCII char, not a null terminator.
- `0x02` UTF-16BE (v2.4 only): similar, high byte first.

---

## Virgil Notes

- Range slicing: `data[pos ..+ len]` (start + length). The `[start ... end]`.
- `int.view(u32_val)` reinterprets bits; `u32.!(byte_val)` widens. Used in
  `readBE32` to avoid signed-overflow when the high byte ≥ 0x80.
- Top-level `def NAME: Array<T> = [...]` works for module-level constant tables
  (bitrate/sample-rate lookup tables live here).
- Using StringBuilder for building strings.

---

## What's Next: Audio Frame Decoding

`Mp3AudioData.data` holds the raw bytes of all MPEG frames. The next session
should implement Layer III (MP3) decoding inside this container. Key stages:

1. **Frame iterator** — walk `data` frame by frame using the 4-byte header +
   computed frame size. Each call to `parseAudioFrameHeader` already works.

2. **Side information parsing** (17 or 32 bytes after the 4-byte header):
   - `main_data_begin` (9 bits): back-pointer into the bit reservoir.
   - Per granule (×2 for MPEG-1, ×1 for MPEG-2) per channel: `part2_3_length`,
     `big_values`, `global_gain`, `scalefac_compress`, window/Huffman config.

3. **Bit reservoir** — Layer III main data does not begin immediately after side
   info; `main_data_begin` points backward into previous frames' data. Need a
   circular byte buffer spanning the inter-frame boundary.

4. **Scale factor decoding** — variable-length from the main data bitstream,
   governed by `scalefac_compress` and `scfsi` (scale factor select info).

5. **Huffman decoding** — 32 Huffman tables (from the ISO spec); decode
   `big_values` pairs + `count1` region quads into 576 frequency lines.

6. **Requantization** — apply `global_gain`, `subblock_gain`, `scalefac_scale`,
   `preflag`, and per-scalefactor-band scale factors to each of the 576 lines.

7. **Stereo processing** — mid/side and/or intensity stereo (joint stereo mode).

8. **Reordering** — short-window blocks use a different frequency ordering;
   must be undone before IMDCT.

9. **Alias reduction** — 8 butterfly operations across 18-sample subband pairs
   (long blocks only).

10. **IMDCT** — 36-point (long) or 12-point (short) inverse MDCT per subband,
    producing 32 sets of 18 time-domain samples (576 total per channel/granule).

11. **Polyphase synthesis filter bank** — 32-band synthesis, producing 32 PCM
    samples per subband per slot (= 576 PCM samples per granule total).

Reference: ISO 11172-3 (MPEG-1 Audio); see also the `dist10` reference decoder
and `minimp3` (single-header C, ~1000 lines, excellent reference implementation).
