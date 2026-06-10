# PNG library (`lib/png/`)

Decoder and dumper for the PNG image format, following the same structure as `lib/mp3/` and `lib/gpx/`.

## Files

| File | Purpose |
|------|---------|
| `PngLayouts.v3` | Binary layout definitions for fixed-size PNG structures |
| `PngIr.v3` | Intermediate representation (IR) classes, enums, and types |
| `PngParser.v3` | Parser: reads chunks, decodes known types into the IR |
| `pngdumper.main.v3` | CLI tool for inspecting PNG files |
| `Makefile` | Builds `pngdumper` via `v3c-host` |

## Building

```
make          # builds ./pngdumper
make run FILE=foo.png
make clean
```

## Usage

```
pngdumper [options] <file.png> ...

  --info        Image summary: dimensions, color type, IDAT size (default)
  -c/--chunks   List all chunks with type, byte count, and file offset
  -t/--text     Show tEXt / zTXt / iTXt text metadata
  -m/--meta     Show gamma, sRGB intent, pHYs (with DPI), tIME, cHRM, tRNS, sBIT
  -p/--palette  Show PLTE palette entries as rgb(r, g, b)
```

## PNG format overview

Every PNG file begins with an 8-byte signature (`\x89PNG\r\n\x1a\n`) followed by a sequence of **chunks**. Each chunk has a fixed structure:

```
4 bytes  length    big-endian u32, counts data bytes only
4 bytes  type      ASCII type code (e.g. "IHDR", "IDAT")
N bytes  data      chunk body
4 bytes  CRC-32    of type + data
```

### Critical chunks (always present in valid PNGs)

| Chunk | Size | Contents |
|-------|------|---------|
| `IHDR` | 13 bytes | Width, height, bit depth, color type, compression, filter, interlace |
| `PLTE` | N×3 bytes | Palette (required for indexed color, optional for truecolor) |
| `IDAT` | variable | Deflate-compressed image data (may be split across multiple consecutive chunks) |
| `IEND` | 0 bytes | End-of-file marker |

### Color types (IHDR byte 9)

| Code | Name | Valid bit depths |
|------|------|-----------------|
| 0 | Grayscale | 1, 2, 4, 8, 16 |
| 2 | Truecolor (RGB) | 8, 16 |
| 3 | Indexed | 1, 2, 4, 8 |
| 4 | Grayscale + Alpha | 8, 16 |
| 6 | Truecolor + Alpha (RGBA) | 8, 16 |

### Ancillary chunks decoded by the parser

| Chunk | Size | Contents |
|-------|------|---------|
| `tEXt` | variable | Keyword + null + Latin-1 text |
| `zTXt` | variable | Keyword + null + compression method + deflate-compressed text |
| `iTXt` | variable | Keyword + compression flags + language tag + UTF-8 text |
| `gAMA` | 4 bytes | Gamma × 100000 (e.g. 45455 ≈ 1/γ 2.2) |
| `sRGB` | 1 byte | Rendering intent (0=perceptual, 1=relative, 2=saturation, 3=absolute) |
| `cHRM` | 32 bytes | White point + RGB primaries as CIE xy × 100000 (8 × u32) |
| `pHYs` | 9 bytes | Pixels per unit X/Y + unit (0=unknown, 1=metre) |
| `tIME` | 7 bytes | Last modification time (year u16 + month/day/hour/minute/second) |
| `tRNS` | variable | Transparency data (stored raw; length depends on color type) |
| `sBIT` | 1–4 bytes | Significant bits per channel (stored raw) |

Unknown chunks are stored in `PngImage.chunks` as raw `Range<byte>` slices.

## IR design (`PngIr.v3`)

```
PngImage
  ihdr:         PngIhdr          -- width, height, bitDepth, colorType, interlace
  palette:      PngPaletteEntry[] -- null if no PLTE
  idatSize:     int              -- total compressed bytes across all IDAT chunks
  idatChunks:   int              -- number of IDAT chunks
  texts:        PngText[]        -- all text chunks
  gamma:        int              -- raw × 100000; -1 if absent
  srgbIntent:   int              -- 0-3; -1 if absent
  chrm:         PngChrm          -- null if absent
  phys:         PngPhys          -- null if absent
  time:         PngTime          -- null if absent
  transparency: Range<byte>      -- raw tRNS data; null if absent
  sigBits:      Range<byte>      -- raw sBIT data; null if absent
  chunks:       PngChunk[]       -- all chunks in file order (raw)
```

`PngPaletteEntry` is a value `type` (not a class): `type PngPaletteEntry(r: u8, g: u8, b: u8) {}`.

Chunk data (`PngChunk.data`, `transparency`, `sigBits`) are zero-copy `Range<byte>` slices into the original file buffer.

## Parser design (`PngParser.v3`)

Uses a `DataReader r` (from the Virgil standard library) for sequential stream access.

**Two read families:**

- **Stream reads** — consume bytes from `r`, used only for chunk headers:
  - `readBE32()` — reads 4 bytes via `r.read1()` × 4, returns big-endian int
  - `readStr4()` — reads 4 bytes via `r.read1()` × 4, returns string (chunk type code)

- **Range reads** — fixed-offset access into a `Range<byte>`, used by sub-parsers:
  - `getBE32(d, at)` — big-endian u32 from slice
  - `getBE16(d, at)` — big-endian u16 from slice

**Main loop:**
```
while r.peekN(8):
    chunkOffset = r.pos
    length      = readBE32()          // consumes 4 bytes
    typeCode    = readStr4()          // consumes 4 bytes
    chunkData   = r.data[r.pos ..+ length]   // zero-copy alias
    // decode chunkData based on typeCode
    r.skipN(length + 4)               // skip data body + CRC
```

Sub-parsers receive the `chunkData` slice directly and use `getBE32`/`getBE16` to read fields from it. The `DataReader` is not used inside sub-parsers — only for the top-level chunk stream.

## Notes

- IDAT data is not decompressed; only the total compressed size is tracked.
- `zTXt` and compressed `iTXt` text is not decompressed; the keyword is decoded and the chunk is marked `compressed = true`.
- The `gamma` and `srgbIntent` fields use `-1` as a sentinel for "chunk absent".
- Chunk ordering is not validated (per the PNG spec, ancillary chunks have flexible placement).
- CRC-32 values are not verified.
