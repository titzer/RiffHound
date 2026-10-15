# MuScriptor: An Open Model for Multi-Instrument Music Transcription

**Paper:** https://arxiv.org/abs/2607.08168
**Code:** https://github.com/muscriptor/muscriptor
**Models:** https://huggingface.co/MuScriptor
**Demo:** https://muscriptor.kyutai.org
**Date:** July 2026
**Authors:** Simon Rouard, Michael Krause, Axel Roebel, Carl-Johann Simon-Gabriel, Alexandre Defossez (Kyutai / Mirelo)
**License:** Code=MIT, Weights=CC-BY-NC-4.0

## Summary

MuScriptor is a multi-instrument automatic music transcription (AMT) model that
converts audio recordings into MIDI. It is the first transcription model trained
on a large-scale dataset of 170k real songs (11k hours) spanning classical to
heavy metal, combined with 1.45M synthetic MIDIs. It dramatically outperforms
prior work (YourMT3+) across all metrics.

## Architecture

- **Type:** Decoder-only Transformer (no complex modifications)
- **Input:** 16kHz mono audio -> mel-spectrogram (2048-pt STFT, 160-sample hop,
  100Hz frame rate, 512 mel bins). Processes 5-second chunks.
- **Conditioning:** Optional instrument presence embeddings concatenated with
  spectrogram projections via lookup tables.
- **Tokenization:** MT3-style note tokens, 128 MIDI instruments grouped into
  36 subgroups (MT3_FULL_PLUS taxonomy).
- **Sizes:**
  - Small: 103M params (14 layers, dim=768) - CPU-friendly
  - Medium: 307M params (24 layers, dim=1024, 16 heads) - default
  - Large: 1.4B params (48 layers, dim=1536) - best accuracy, needs GPU

## Training Pipeline

### Stage 1: Synthetic Pre-training
- 1.45M MIDI files synthesized on-the-fly with 250+ random soundfonts
- Augmentations: pitch shift, tempo variation, velocity adjustment, instrument
  randomization, random detuning
- Establishes basic note-tracking capability

### Stage 2: Fine-tuning on Real Audio
- 170k recordings with aligned note annotations
- Alignment via DTW with chroma/onset features + quality filtering
- 1M steps, batch size 64, AdamW (lr=1e-4, cosine schedule)
- Conditioning dropout 0.2 (for classifier-free guidance at inference)

### Stage 3: Reinforcement Learning (GRPO)
- 300 manually-verified high-quality tracks
- G=8 independent transcriptions per segment at temperature 0.75
- Reward = F_onset + F_frame + F_offset (note-level F-scores)
- Group-relative advantage normalization (GRPO-inspired)

## Performance (1.3B model vs YourMT3+)

| Metric      | YourMT3+ | MuScriptor | Improvement |
|-------------|----------|------------|-------------|
| Onset F1    | 32.5     | 60.4       | +86%        |
| Frame F1    | 45.5     | 72.4       | +59%        |
| Offset F1   | 17.8     | 48.6       | +173%       |
| Drums F1    | 41.4     | 49.6       | +20%        |
| Multi F1    | 21.9     | 47.8       | +118%       |

Medium model (307M): Onset=52.4, Frame=68.0, Offset=40.3, Drums=42.0, Multi=39.7

## Supported Instruments (36 groups)

Top instruments in training data by coverage:
- Acoustic piano (51%), Distorted e-guitar (47%), E-bass (43%)
- Plus: drums, acoustic guitar, clean e-guitar, strings, synths, brass,
  woodwinds, organ, voice, etc.

## Key Technical Details

- **Classifier-Free Guidance:** alpha=2 at inference improves accuracy
- **Instrument Conditioning:** Track-level (not segment-level) instrument
  presence stabilizes predictions across chunk boundaries
- **Mel-spectrograms beat learned representations:** Outperformed CQT, Encodec,
  and MERT embeddings
- **Scaling:** Predictable improvement with model size (65 -> 73.3 frame F1
  from 60M to 1.3B)

## Limitations

- No velocity/dynamics prediction
- Cannot represent two identical pitch+instrument notes simultaneously
- Lower precision on choral music and dense mixes
- Biased toward pop and Western classical
- Drums are onset-only (no offset/duration)

## Relevance to RiffHound

### Direct Applications
1. **Automatic transcription of playalong songs** - Generate MIDI for the entire
   library, enabling note-level display during practice
2. **Instrument-specific parts** - Extract guitar, bass, drums separately for
   isolated practice or virtual band member generation
3. **Chord/note detection** - Complement existing chroma analysis with note-level
   precision (pitches, onsets, offsets)
4. **Backing track intelligence** - Know exactly what each instrument plays so
   virtual band members can fill gaps

### Integration Ideas
- Run offline batch transcription of playalong library -> store MIDI alongside audio
- Use streaming API for real-time transcription during recording/playback
- Feed MIDI data into RiffText notation format for display
- Combine with beat detection for bar-aligned transcriptions
- Instrument conditioning: transcribe only the guitar part for guitar lessons

### Considerations
- CC-BY-NC license means non-commercial use only for model weights
- Small model feasible on CPU for batch processing
- Medium/Large need GPU for reasonable speed
- 5-second chunk processing means slight latency for real-time use
- Output is MIDI tokens, would need conversion to RiffText or display format
