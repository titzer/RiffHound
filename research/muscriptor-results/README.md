# MuScriptor Transcription Results

Model: muscriptor-small (103M params)
Device: CPU (Apple M-series ARM64)
Date: 2026-07-19

## Performance Summary

| Song | Duration | Chunks | Transcription Time | Realtime Factor | Total Notes | Instruments Detected |
|------|----------|--------|-------------------|-----------------|-------------|---------------------|
| Blackbird (Beatles) | 138s | 28 | 22s | 6.3x | 931 | acoustic guitar, drums, chromatic percussion |
| Iron Man (Black Sabbath) | 359s | 72 | 106s | 3.4x | 5287 | drums, distorted e-guitar, e-bass, voice |
| Bohemian Rhapsody (Queen) | 356s | 72 | 159s | 2.2x | 4933 | voice, e-bass, drums, piano, strings, acoustic guitar, flutes, distorted e-guitar, clarinet, synth, trumpet, clean e-guitar |
| Comfortably Numb (Pink Floyd) | 384s | 77 | 138s | 2.8x | 8231 | e-bass, strings, drums, acoustic guitar, voice, distorted e-guitar |
| Hey Joe (Hendrix) | 210s | 42 | 87s | 2.4x | 4486 | distorted e-guitar, e-bass, drums, voice |

## Observations

### Instrument Detection
- Correctly identifies primary instruments in each song
- Blackbird: properly detected as acoustic guitar (fingerpicking)
- Iron Man: correctly picks up distorted guitar, bass, drums
- Bohemian Rhapsody: impressively detects 12 instrument groups including piano, strings, flutes
- Comfortably Numb: identifies string ensemble (orchestral backing) and guitar parts
- Hey Joe: distorted electric guitar dominant (2756 notes) - correct for Hendrix

### Speed
- On CPU (small model): 2.2x to 6.3x realtime
- Simpler songs (fewer instruments/notes) transcribe faster
- Bohemian Rhapsody slowest due to dense multi-instrument arrangement

### Limitations Observed
- m4a (AAC) files not supported - libsndfile can't decode them
  - Workaround: convert to mp3/wav/flac first (many playalong songs are m4a)
- Voice detection is sparse (17-134 notes) - model treats voice as pitched instrument
  but vocal melodies are hard to capture as discrete MIDI notes
- No velocity information in output (known limitation)

## File Format Notes
- Output: Standard MIDI files with one track per detected instrument
- Each track has a program_change to set the General MIDI instrument
- Can be opened in any DAW, MIDI player, or notation software
- Drum track uses channel 10 convention

## Next Steps for RiffHound Integration
1. Batch-convert m4a files to mp3/wav for full library coverage
2. Run medium model (with GPU) for better accuracy on complex songs
3. Use instrument conditioning to get cleaner per-instrument parts
4. Map MIDI output to RiffText notation for display in webui
5. Use note onsets for beat-aligned chord detection (complement chroma)
