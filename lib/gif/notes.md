# GIF library (`lib/gif/`)

Decoder and dumper for the GIF image format (GIF87a and GIF89a), following the same structure as `lib/png/`, `lib/mp3/`, and `lib/gpx/`.

## Files

| File | Purpose |
|------|---------|
| `GifLayouts.v3` | Binary layout definitions for fixed-size GIF structures |
| `GifIr.v3` | Intermediate representation (IR) classes, enums, and types |
| `GifParser.v3` | Parser: reads all blocks, decodes known extension types into the IR |
| `gifdumper.main.v3` | CLI tool for inspecting GIF files |

## Usage

```
gifdumper [options] <file.gif> ...

  --info          File summary: version, canvas size, frame count, color table, loop (default)
  -f/--frames     List all image frames: position, size, delay, disposal, transparency
  -e/--exts       List all extension blocks with labels and byte counts
  -p/--palette    Show global color table entries as hex + rgb(r, g, b)
  -c/--comments   Show all comment extension text
  -a/--animation  Per-frame delay table in centiseconds and milliseconds
```

---

## GIF format overview

GIF (Graphics Interchange Format) was designed by CompuServe in 1987 (GIF87a) and extended in 1989 (GIF89a). GIF89a added extension blocks enabling animation, transparency, comments, and application-defined metadata.

All multi-byte integers are **little-endian**. Color values are always 8 bits per channel.

### File layout

```
6 bytes   Header              "GIF87a" or "GIF89a"
7 bytes   Logical Screen Descriptor
N×3 bytes Global Color Table  (optional; present if LSD flag set)

[ blocks, in any order, until trailer: ]
  0x2C    Image Descriptor    (9 bytes) + optional Local Color Table + image data sub-blocks
  0x21    Extension Introducer
    0xF9  Graphic Control Extension
    0xFE  Comment Extension
    0x01  Plain Text Extension
    0xFF  Application Extension
    ...   (other labels reserved)
0x3B      Trailer
```

### Logical Screen Descriptor (7 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 2 | Canvas width (LE u16) |
| 2 | 2 | Canvas height (LE u16) |
| 4 | 1 | Packed byte (see below) |
| 5 | 1 | Background color index |
| 6 | 1 | Pixel aspect ratio (0 = not given; actual ratio = (N+15)/64) |

Packed byte:

```
bit 7    : global color table flag
bits 4-6 : color resolution minus 1 (bits per primary color channel, 1–8)
bit 3    : sort flag (table sorted by decreasing importance)
bits 0-2 : global color table size field N  →  table has 2^(N+1) entries
```

### Color tables

Each entry is 3 bytes: R, G, B (one byte each, 0–255). The number of entries is always a power of two: 2^(sizeField+1), giving 2, 4, 8, 16, 32, 64, 128, or 256 entries. The global color table immediately follows the Logical Screen Descriptor if its flag is set. A local color table immediately follows the Image Descriptor if its flag is set, and overrides the global table for that frame.

### Image Descriptor (introduced by 0x2C, then 9 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 2 | Left offset within canvas (LE u16) |
| 2 | 2 | Top offset within canvas (LE u16) |
| 4 | 2 | Frame width (LE u16) |
| 6 | 2 | Frame height (LE u16) |
| 8 | 1 | Packed byte (see below) |

Packed byte:

```
bit 7    : local color table flag
bit 6    : interlace flag
bit 5    : sort flag
bits 3-4 : reserved (0)
bits 0-2 : local color table size field N  →  2^(N+1) entries
```

After the (optional) local color table:
- 1 byte: LZW minimum code size (typically 2 for indexed images)
- Sub-blocks: LZW-compressed pixel indices, terminated by a 0x00 block

### Sub-block encoding

GIF data streams (image data, extension bodies) are split into **sub-blocks**. Each sub-block is a 1-byte count (1–255) followed by that many data bytes. A count of 0x00 is the block terminator. Sub-blocks allow streaming encoders to avoid seeking back to fill in lengths.

```
[count] [data × count]  [count] [data × count]  ...  [0x00]
```

The parser flattens sub-block sequences into contiguous byte arrays, stripping the count bytes.

### Extension blocks (GIF89a, introduced by 0x21)

#### Graphic Control Extension (label 0xF9)

Controls how the following image frame is displayed. Structure:

```
0x21 0xF9          introducer + label
0x04               block size (always 4)
packed (1 byte)    bits 3-5: disposal method; bit 2: user input; bit 0: transparency flag
delay  (2 bytes)   delay in 1/100 seconds (LE u16)
trans  (1 byte)    transparent color index (valid only if transparency flag set)
0x00               block terminator
```

**Disposal methods:**

| Code | Name | Meaning |
|------|------|---------|
| 0 | None | No disposal specified |
| 1 | Do not dispose | Leave frame in place |
| 2 | Restore to background | Fill with background color |
| 3 | Restore to previous | Revert to canvas state before this frame |

#### Comment Extension (label 0xFE)

One or more sub-blocks of arbitrary text, terminated by 0x00. No fixed header. Intended for plain-text comments embedded in the file.

#### Plain Text Extension (label 0x01)

```
0x21 0x01          introducer + label
0x0C               block size (always 12)
grid_left   (2)    LE u16
grid_top    (2)    LE u16
grid_width  (2)    LE u16
grid_height (2)    LE u16
cell_width  (1)    character cell width in pixels
cell_height (1)    character cell height in pixels
fg_index    (1)    foreground color index
bg_index    (1)    background color index
[sub-blocks]       ASCII text, one character per color table entry's cell
0x00               terminator
```

Rarely used in practice; most decoders ignore it.

#### Application Extension (label 0xFF)

```
0x21 0xFF          introducer + label
0x0B               block size (always 11)
app_id   (8)       application identifier (ASCII, space-padded)
auth_code (3)      application authentication code
[sub-blocks]       application-specific data
0x00               terminator
```

The most common application extension is **NETSCAPE 2.0**, which encodes an animation loop count:

```
app_id    = "NETSCAPE"
auth_code = "2.0"
sub-block: 0x01  [loop_count_lo]  [loop_count_hi]
```

Loop count 0 means loop forever; 1–65535 means repeat N times.

### Interlacing (Adam4)

When the interlace flag is set in an Image Descriptor, the LZW-compressed stream encodes rows in four passes rather than top-to-bottom:

| Pass | Rows | Step |
|------|------|------|
| 1 | 0, 8, 16, 24, … | every 8th row starting at 0 |
| 2 | 4, 12, 20, 28, … | every 8th row starting at 4 |
| 3 | 2, 6, 10, 14, … | every 4th row starting at 2 |
| 4 | 1, 3, 5, 7, … | every 2nd row starting at 1 |

The parser records the flag but does not reorder rows.

---

## IR design (`GifIr.v3`)

```
GifFile
  version:      GifVersion              -- GIF87a or GIF89a
  screen:       GifScreen               -- logical screen descriptor (never null)
  globalTable:  GifColorEntry[]         -- null if no global color table
  frames:       GifFrame[]              -- image frames in file order
  comments:     GifComment[]            -- comment extensions
  plainTexts:   GifPlainText[]          -- plain text extensions
  appExts:      GifAppExtension[]       -- application extensions (raw + decoded app id)
  netscape:     GifNetscapeExt          -- null if no NETSCAPE 2.0 extension
  extensions:   GifExtension[]          -- all extensions (raw) in file order
  animated:     bool                    -- true if frames.length > 1
  totalFrames:  int                     -- frames.length

GifScreen
  width, height:    int                 -- canvas dimensions
  hasGlobalTable:   bool
  colorResolution:  int                 -- bits per channel minus 1 (field value, not actual bits)
  globalSorted:     bool
  globalTableSize:  int                 -- entry count (2^(N+1))
  bgColorIndex:     u8
  pixelAspect:      u8                  -- 0 = not given

GifFrame
  index:          int                   -- 0-based frame number
  left, top:      int                   -- offset within canvas
  width, height:  int                   -- frame pixel dimensions
  hasLocalTable:  bool
  interlaced:     bool
  localSorted:    bool
  localTable:     GifColorEntry[]       -- null if no local color table
  lzwMinCode:     u8                    -- LZW minimum code size
  imageDataSize:  int                   -- total compressed bytes (sub-block data only)
  imageOffset:    int                   -- file offset of the 0x2C separator byte
  graphicCtrl:    GifGraphicControl     -- null if no preceding GCE

GifGraphicControl
  disposal:          GifDisposal
  userInput:         bool
  hasTransparency:   bool
  delayCs:           int                -- delay in 1/100ths of a second
  transparentIndex:  u8

GifColorEntry  (type, not class)
  r, g, b:  u8
```

`GifColorEntry` is a value `type` (not a `class`): `type GifColorEntry(r: u8, g: u8, b: u8) {}`.

Application extension data (`GifAppExtension.data`) and comment/plain-text body (`GifComment.text`, `GifPlainText.text`) are **copied** out of the file buffer because sub-block sequences are not contiguous in the source data (each sub-block has a size prefix byte that must be stripped).

---

## Parser design (`GifParser.v3`)

Uses `DataReader r` for sequential stream access (same as `PngParser`).

### Sub-block discipline

All extension parsers follow a strict convention:

- Each parser reads exactly its **fixed-size header block** (the block-size byte plus that many data bytes).
- Any trailing **sub-block sequence** is always handled by the **caller** via `collectSubBlocks()` or `skipSubBlocks()`.
- Exception: the Graphic Control Extension has no sub-blocks — its body is exactly 4 bytes followed by a single 0x00 terminator, which `parseGCE()` reads itself.

This keeps the sub-block logic in one place and makes error paths clean: if a fixed header is malformed, the parser skips the fixed block bytes and returns null; the caller then calls `collectSubBlocks()` unconditionally.

### `collectSubBlocks()` implementation

Uses a two-pass approach to avoid over-allocating:

1. **First pass** — walk sub-blocks via direct `r.data[p]` indexing (no stream reads) to count total data bytes. Record the end position `p`.
2. **Second pass** — allocate `Array<byte>.new(total)` and copy, skipping size bytes.
3. Advance the stream with `r.skipN(p - start)`.

This is necessary because GIF sub-blocks interleave size bytes with data bytes, so a contiguous slice of `r.data` would include unwanted size bytes.

### Main block loop

```
while r.peekN(1):
    blockOffset = r.pos
    intro       = r.read1()

    0x3B → break (trailer)

    0x2C → Image Descriptor
        parseImageDescriptor(index, blockOffset, pendingGCE)
        pendingGCE = null

    0x21 → extension:
        label = r.read1()
        0xF9 → parseGCE()                            // no sub-blocks
        0xFE → collectSubBlocks() → GifComment
        0x01 → parsePlainText() + collectSubBlocks() → GifPlainText
        0xFF → parseAppExtension() + collectSubBlocks() → GifAppExtension
             → tryDecodeNetscape() if NETSCAPE 2.0
        else → collectSubBlocks() (unknown extension)

    else → break (unknown introducer)
```

The `pendingGCE` variable holds the most recently parsed Graphic Control Extension and is attached to the next Image Descriptor or Plain Text Extension. It is cleared when consumed.

### `GifFrame.imageDataSize`

Image data sub-blocks are skipped (not stored) via `skipAndCountSubBlocks()`. Only the total compressed byte count is kept. LZW decompression is not implemented.

---

## Notes

- LZW image data is not decompressed; only the total compressed byte count per frame is tracked.
- Interlaced images are flagged but rows are not reordered.
- The Pixel Aspect Ratio field is stored raw; actual ratio is `(raw + 15) / 64`.
- GIF87a files may still contain extension blocks in practice (some encoders write them anyway); the parser accepts them regardless of version.
- The NETSCAPE 2.0 extension is the only application extension with explicit decoded support; all others are stored as `GifAppExtension` with raw `data: Range<byte>`.
- GIF uses a global/local color table model rather than per-pixel color: each pixel is an index (0–255) into the active table. There is no alpha channel; transparency is represented by designating one palette index as transparent via the Graphic Control Extension.
- The color resolution field in the Logical Screen Descriptor records the original hardware color depth of the source image, not the color table size. It is informational only.
