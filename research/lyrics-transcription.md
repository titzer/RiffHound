# Lyrics transcription and lyrics-to-audio alignment for beatmapper

Research date: 2026-10-11. Scope: models that turn a sung vocal stem into text
(automatic lyrics transcription, ALT) and models that place known lyric text
in time (forced alignment), judged for beatmapper's setup. That setup is a
native C++ app on Apple Silicon (M4 Pro), with Demucs `htdemucs_6s` vocal
stems already cached, and a `LyricMap` of `(t_start, t_end, text)` lines that
starts out with plain, untimed lyric text.

## 1. Recommendation

**Primary: a hybrid built on the Qwen3 speech stack, run as a Python/MLX
sidecar like `stems_demucs.py`.** It has three steps:

1. **Find phrases.** Split the Demucs vocal stem into phrases with an RMS
   voice-activity detector (VAD). We already have the stem, so this needs no
   model.
2. **Transcribe each phrase.** Run **Qwen3-ASR (0.6B, or 1.7B for accuracy)**
   on each phrase to get rough text.
3. **Match, then align.** In C++, fuzzy-match the phrase texts to the known
   lyric lines with a monotonic DP. Then run **Qwen3-ForcedAligner-0.6B** on
   each matched (phrase audio, lyric line text) pair to get word timestamps.

Why this one:

- **Accuracy.** Qwen3-ASR is the best open model on singing that is
  published so far. Against Whisper-large-v3, its WER is 5.98 vs 13.58 on
  M4Singer, 6.25 vs 11.71 on MIR-1k-vocal and 8.52 vs 13.77 on Popcs. It also
  scores 14.6 % WER on whole English songs with backing music, where Whisper
  has no long-form song number at all
  ([Qwen3-ASR tech report, Table 7](https://arxiv.org/html/2601.21337)).
- **Timing accuracy.** The paired aligner's average word shift on
  human-labelled data is 32 ms, against 101 ms for NeMo NFA and 133 ms for
  WhisperX on MFA-labelled data. Its error also stays low on 300 s audio,
  where WhisperX drifts to 2.7 s (same report, Table 9).
- **License.** Both models are **Apache-2.0**
  ([report](https://arxiv.org/html/2601.21337)). The usual CTC aligner
  weights are non-commercial (MMS is CC-BY-NC); this avoids that problem.
- **Apple Silicon.** A native MLX port exists:
  [`mlx-qwen3-asr`](https://github.com/moona3k/mlx-qwen3-asr/) (Apache-2.0,
  `pip install mlx-qwen3-asr[aligner]`). It reports RTF 0.029 for 0.6B fp16
  and 0.077 for 1.7B fp16 on an M4 Pro. That is under 10 s of compute for a
  4-minute song, comparable to our Demucs pass. Swift and Core ML ports also
  exist
  ([speech-swift](https://github.com/soniqo/speech-swift),
  [Qwen3-ForcedAligner Core ML](https://huggingface.co/aufklarer/Qwen3-ForcedAligner-0.6B-CoreML-FP16)),
  which leaves a route to embedding it later.
- **Why match before aligning.** Real lyric sheets differ from what is sung:
  choruses written once but sung three times, ad-libs, missing lines, a
  different version of the song. Global forced alignment of the whole sheet
  breaks on exactly these cases. Matching each phrase's transcript against
  the sheet first fixes that, because it decides *which* line goes *where*.
  The aligner then only has to do the easy part: placing words inside a
  window of a few seconds whose text is already right.

**Fallback / embedded option: whisper.cpp.** Use large-v3 or large-v2 with
Metal (and a Core ML encoder if wanted), the language forced, VAD
segmentation, and DTW token timestamps
([whisper.cpp](https://github.com/ggml-org/whisper.cpp), MIT). It links
straight into the C++ binary and needs no venv. For singing, its word timing
and WER are clearly worse than the Qwen stack. Still, phrase-level text from
it is good enough to drive the fuzzy line matching, which is the part that
matters most. Choose this if a second Python venv is unwelcome, or if the
Qwen ports turn out unstable.

**Should we align or transcribe, given that we know the lyrics?** Alignment
gives far better *timing*. Purpose-built lyrics aligners reach about 0.2 s
mean and 0.04 s median absolute error on JamendoLyrics
([Spotify 2023](https://arxiv.org/pdf/2306.07744),
[AIST DAFx 2025](https://staff.aist.go.jp/m.goto/PAPER/DAFX2025cheng.pdf)).
Raw Whisper segment times can be off by seconds
([WhisperX README](https://github.com/m-bain/whisperX)). But alignment alone
assumes the text is exactly what was sung, in order, once. So use
transcription to anchor lines and alignment to refine them. That is the
hybrid above. If a sheet is known to be exact and complete, you can skip the
ASR and align directly. Even then, keep the ASR pass as a confidence check.

## 2. Comparison table

| Option | Task | Singing quality (evidence) | Word timestamps | License (code / weights) | Size | Apple Silicon path | Embed in C++? | Languages |
|---|---|---|---|---|---|---|---|---|
| **Qwen3-ASR 0.6B / 1.7B** | ALT | Best open model: 1.7B gets 6-8.5 % WER on solo-singing sets and 14.6 % on full EN songs with BGM ([report](https://arxiv.org/html/2601.21337)). One independent run found 27 % on Demucs vocals vs 22 % for large-v3 ([lyrics-bench, unofficial](https://lyrics-bench.onrender.com/papers/headroom_report.html)). **Needs a local bench.** | Via ForcedAligner | Apache-2.0 / Apache-2.0 | 0.6B, 1.7B | MLX (RTF 0.03-0.08 on M4 Pro), Core ML/Swift ports | Not directly (MLX-Swift/Core ML possible) | 30 languages + 22 Chinese dialects |
| **Qwen3-ForcedAligner 0.6B** | Alignment | 32 ms average word shift on human-labelled speech; no singing benchmark published | Yes (word/char, 80 ms frames) | Apache-2.0 | 0.6B | MLX, Core ML FP16 | Core ML possible | 11 (zh, en, yue, fr, de, it, ja, ko, pt, ru, es) |
| **Whisper large-v2 / v3 (OpenAI)** | ALT | Jam-ALT full mix: about 35.5 % WER (v3) and 35.7 % (v2) out of the box ([Jam-ALT](https://arxiv.org/pdf/2311.13987)). 20.35 % with VAD segmentation ([2506.15514](https://arxiv.org/html/2506.15514v1)). About 15 % on clean MUSDB vocal stems | Cross-attention DTW (approximate) | MIT / MIT | 1.55B | via whisper.cpp / mlx-whisper | **Yes** (whisper.cpp) | 99 |
| **Whisper large-v3-turbo** | ALT | Reported to be worse on music than large-v2/v3 ([lyrics-bench brief, unverified](https://lyrics-bench.onrender.com/papers/research_brief.html)) | DTW | MIT / MIT | 809M | whisper.cpp, MLX | Yes | 99 |
| **whisper.cpp** | Runtime | same as model | `-ml 1` word splits, DTW `t_dtw` per token | MIT | GGML, quantizable | Metal GPU, Core ML encoder on the ANE (>3x faster than CPU) | **Native C/C++ library** | per model |
| **mlx-whisper** | Runtime | same as model | `word_timestamps=True` | MIT (mlx-examples) | | MLX GPU | No (Python) | per model |
| **faster-whisper** | Runtime | used by the 2025 SOTA pipeline | Yes | MIT | CTranslate2 | **CPU only on Mac** (no Metal) | No | per model |
| **WhisperX** | ALT + wav2vec2 alignment | Whisper + VAD, then CTC re-alignment | Yes (wav2vec2) | BSD-2 / per-language wav2vec2 | | No MPS mention, CPU on Mac ([README](https://github.com/m-bain/whisperX)) | No | Alignment: en, fr, de, es, it built in, others from HF |
| **ctc-forced-aligner (MMS-300M)** | Alignment | Speech-trained CTC; works on clean vocal stems, with `<star>` tokens absorbing intros and gaps | Yes | BSD-2 / **CC-BY-NC-4.0** ([model card](https://huggingface.co/MahmoudAshraf/mms-300m-1130-forced-aligner)) | 300M | PyTorch MPS; ONNX and Core ML conversions exist ([ONNX](https://huggingface.co/tatsu020/mms-300m-fa-onnx), [Core ML](https://huggingface.co/chordai/mms-fa-aligner-coreml)) | Yes via ONNX Runtime + our own Viterbi (about 100 lines) | 1100+ (with romanization) |
| **torchaudio `forced_align` / MMS_FA** | Alignment | as above | Yes | BSD / CC-BY-NC | 300M | | | **Removed in torchaudio 2.9** ([tutorial note](https://docs.pytorch.org/audio/2.9.0/tutorials/ctc_forced_alignment_api_tutorial.html)) |
| **NeMo Forced Aligner** | Alignment | Speech CTC; 101 ms average shift (speech) | Yes | Apache-2.0 / per model | | Heavy NeMo dependency, CUDA-oriented | No | per model |
| **Montreal Forced Aligner** | Alignment | Speech acoustic models fit singing poorly unless adapted ([NHSS](https://arxiv.org/pdf/2012.00337)) | Yes (phone/word) | MIT | small (Kaldi GMM) | CPU | No (Kaldi/conda) | dictionary-based languages |
| **schufo/lyrics-aligner** | Lyrics alignment | Trained on MUSDB18 + lyrics; joint separation, works on mixes | Phoneme/word onsets | **MIT** ([repo](https://github.com/schufo/lyrics-aligner)) | small | CPU/PyTorch | Possible (small net) | English only (CMU ARPAbet) |
| **SOFA / SingAlign** | Singing phoneme alignment | Built for solo singing (SVS data prep) | Phoneme | SOFA weights CC-BY-NC-SA ([HF](https://huggingface.co/Silasimo/SOFA-combined)) | small | CPU | Possible | mostly zh/ja/en |
| **AudioShake LyricSync** | ALT + alignment (commercial) | 16.1 % Jam-ALT WER, the best published; claims >95 % alignment accuracy | Yes | Commercial API/SDK ([docs](https://developer.audioshake.ai/lyrics/overview)) | n/a | macOS SDK | SDK | 40+ |
| **Music Flamingo / SALMONN / Qwen2.5-Omni** | Audio LLMs | Music Flamingo 8B: 19.6 % on MUSDB18 EN (secondary source). Gemini 2.5 Pro 12.2 % (vendor eval) | No | various, mostly NC or large | 7-8B+ | Heavy | No | |

## 3. Notes per option

### Whisper family

- **On lyrics.** Jam-ALT
  ([Cífka et al.](https://arxiv.org/pdf/2311.13987)) found large-v3 no better
  than large-v2: 35.5 vs 35.7 % WER on full mixes. HTDemucs-separated vocals
  often *hurt*, partly because separated vocals made Whisper detect the wrong
  language. **Always pass the language explicitly.**
- **The 2025 open-source SOTA**
  ([Exploiting Music Source Separation for ALT with Whisper, arXiv 2506.15514](https://arxiv.org/html/2506.15514v1))
  used faster-whisper large-v2 with beam 5 and the language forced. Its gains
  come mostly from *where audio is cut*: RMS-VAD on separated vocals
  ("cut & merge", 0.1 threshold, 1 s minimum silence, 30 s maximum segment).
  Results:
  - Jam-ALT: 23.02 to 20.35 % WER.
  - MUSDB-ALT with real vocal stems: 14.98 %.
  - Hallucination bursts (runs of 10+ inserted words) dropped 3-9x. The
    authors think cuts through active singing trigger hallucinations.
  - Whisper "systematically deletes non-lexical vocables and backing
    vocals", dropping over half of them.

  Our situation is close to "good separated vocals", so we should land
  somewhere between their 15 and 21 % numbers. Phrase segmentation from our
  stem's RMS is the key technique, and we already have the stem.
- **Hallucination.** large-v3 emits text for about 40 % of non-speech inputs
  and loops on repeated text
  ([arXiv 2501.11378](https://arxiv.org/html/2501.11378v1)). Mitigations:
  - Only feed segments where the VAD says there is singing.
  - Set `condition_on_previous_text=False` (whisper.cpp: `-mc 0`).
  - Use temperature 0 with fallback.
  - Drop segments whose compression ratio or average log-probability is out
    of range.
  - Optionally prompt with the known lyrics. This biases the output toward
    the right vocabulary, but makes hallucinating sheet text more likely, so
    treat it as an experiment.
- **large-v3-turbo.** Faster (4 decoder layers), but reported worse on music.
  Use it only for a fast preview.
- **Word timestamps.** Whisper's own timestamps come from cross-attention DTW
  (whisper.cpp `dtw_token_timestamps` with an alignment-head preset; openai
  `word_timestamps=True`). They are approximate, often within a few hundred
  ms, and degrade on sustained sung vowels. WhisperX exists precisely because
  of this ([README](https://github.com/m-bain/whisperX)).
- **Runtimes on Mac.**
  - **whisper.cpp**: MIT, Metal plus a Core ML encoder, quantized GGML,
    built-in Silero VAD, C API. This is the only Whisper runtime we can link
    in directly.
  - **mlx-whisper**: Python, fast on the GPU.
  - **faster-whisper**: CTranslate2, which has no Metal backend, so CPU only.
  - **WhisperX**: CPU on macOS, and pulls in pyannote.
- **Fine-tunes.** There is no widely adopted general-purpose singing Whisper
  checkpoint on Hugging Face. Published ones are language-specific
  ([brief](https://lyrics-bench.onrender.com/papers/research_brief.html)).
  The relevant lyrics data is MulJam (from
  [LyricWhiz](https://ismir2023program.ismir.net/static/posters/117.pdf),
  381.9 h) and DALI, with an estimated gain of about 3 points from
  fine-tuning. LyricWhiz itself is Whisper plus GPT post-editing (23.7 %
  English WER on Jam-ALT).
  [Interspeech 2025 (Huang et al.)](https://www.isca-archive.org/interspeech_2025/huang25_interspeech.html)
  adds LoRA plus a vocal/mixture consistency loss. That is a research option,
  not off-the-shelf.

### Qwen3-ASR / Qwen3-ForcedAligner (Jan 2026)

- **Sizes.** The 0.6B model uses a 180M AuT encoder and the 1.7B model a
  300M one. Input limit is 1200 s for ASR and **300 s for the aligner**. Our
  per-phrase or per-section alignment stays far under that, but a whole-song
  call needs chunking.
- **Singing results** are the best open numbers published (see table). One
  caution: an independent 12-song test on htdemucs vocals measured Qwen3-ASR
  at 27.4 % against large-v3 at 22.1 %
  ([lyrics-bench headroom report](https://lyrics-bench.onrender.com/papers/headroom_report.html)).
  Vendor numbers may not carry over to separated stems. **Bench both on our
  annotated tracks before committing.** The integration below lets the ASR
  model be swapped.
- **The aligner** is non-autoregressive (one forward pass), 80 ms
  resolution, word or character level, 11 languages. Its training labels
  were MFA alignments of *speech*. It has not been published on singing, so
  long held vowels and melisma are the risk. Phrase-local windows limit how
  much damage that can do.
- **Ports.**
  - [mlx-qwen3-asr](https://github.com/moona3k/mlx-qwen3-asr/): Python and
    MLX, Apache-2.0. JSON/SRT/TSV output, word timestamps via
    `--timestamps`, quantized 4/8-bit variants.
  - [mlx-audio-plus](https://github.com/DePasqualeOrg/mlx-audio-plus):
    `mlx-community/Qwen3-ForcedAligner-0.6B-8bit`.
  - [speech-swift](https://github.com/soniqo/speech-swift): Swift and Core ML.
  - OminiX-MLX: Rust with no Python, 22-30x real time.

  These are community ports, not official releases.

### Forced alignment with known text

- **CTC + Viterbi** (MMS, wav2vec2, NeMo, ctc-forced-aligner) is simple and
  robust. You need per-frame character log-probabilities from an acoustic
  model and a standard trellis. Use a wildcard `<star>` token at the edges
  and between lines so instrumental gaps and ad-libs can be absorbed.
  - [ctc-forced-aligner](https://github.com/MahmoudAshraf97/ctc-forced-aligner)
    is the convenient package: BSD-2 code, about 5x less memory than
    torchaudio, `--star_frequency edges|segment`, romanization for 1100+
    languages.
  - Its default MMS weights are **CC-BY-NC-4.0**. That is fine for personal
    or research use, but a blocker if beatmapper ever ships commercially.
    English-only Apache/MIT wav2vec2 CTC checkpoints (for example
    `facebook/wav2vec2-large-960h-lv60-self`) are a commercial-safe swap with
    worse robustness.
  - torchaudio's `forced_align` was **deprecated in 2.8 and removed in 2.9**,
    so don't build on it.
  - Embedding path: export the acoustic model to ONNX or Core ML (both
    exist for MMS-300M), run it from C++, and do the Viterbi ourselves.
- **[NeMo Forced Aligner](https://docs.nvidia.com/nemo/speech/latest/tools/nemo_forced_aligner.html)**:
  good on long audio, but NeMo is a heavy, CUDA-first dependency. Not worth
  it on a Mac.
- **Montreal Forced Aligner**: a GMM-HMM pronunciation-dictionary aligner
  trained on speech. It needs adaptation for singing (NHSS had to do
  speaker-adaptive training on solo singing), and is conda/Kaldi heavy. Skip.
- **[schufo/lyrics-aligner](https://github.com/schufo/lyrics-aligner)**
  (Schulze-Forster et al., "Phoneme level lyrics alignment and text-informed
  singing voice separation"): MIT, small, trained on singing, outputs
  phoneme and word onsets. It is English-only (CMU ARPAbet, and needs a
  grapheme-to-phoneme step), aligns a whole song globally, and is an older
  research codebase (2021). It is the only commercial-safe aligner trained on
  *singing*, so it is worth keeping as a comparison point.
- **Research lyrics aligners**
  ([Spotify contrastive, 2023](https://arxiv.org/pdf/2306.07744): mean
  absolute error under 0.2 s on Jamendo, multilingual;
  [AIST DAFx 2025](https://staff.aist.go.jp/m.goto/PAPER/DAFX2025cheng.pdf):
  0.216 s mean and 0.041 s median). These are the accuracy ceiling, but have
  no usable released inference package.
- **Singing-voice-synthesis aligners** (SOFA, SingAlign, STARS) target clean
  solo phoneme labelling for synthesis datasets. They are mostly
  Chinese/Japanese, the SOFA weights are non-commercial, and they are not
  built for pop lyrics.

### Commercial and audio-LLM options

- **[AudioShake LyricSync](https://developer.audioshake.ai/lyrics/overview)**:
  separation, then transcription and word alignment, 40+ languages, and a
  macOS SDK. It holds the best published Jam-ALT WER (16.1 %) and has an
  alignment-only mode that takes our known text. It is paid and closed, but
  the useful reference if we ever want to buy rather than build.
- **Audio LLMs** (Gemini 2.5 Pro, Music Flamingo, SALMONN, Qwen2.5-Omni):
  strong reported WER for Gemini, but no timestamps, large models, and mostly
  cloud or 7B+. Not a fit for on-device phrase timing. One cheap use: an LLM
  "reconciliation" pass that merges several ASR hypotheses reached 15.9 % in
  the lyrics-bench study. Our fuzzy matcher against the known lyrics plays
  the same role more cheaply.

## 4. Integration plan

### Shape

This mirrors the Demucs setup in [[beatmapper-stems-setup]]:

- **`make lyrics-model`** builds `external/venv-lyrics` (uv, Python 3.12):
  `mlx-qwen3-asr[aligner]`, `numpy`, `soundfile`. Use a separate venv, since
  MLX and torch don't need to share one. The models download into the
  Hugging Face cache on first run. That is about 1.2 GB for 0.6B ASR plus the
  aligner in fp16, or roughly half with 8-bit weights.
- **`scripts/lyrics_asr.py VOCALS.wav --lang en [--lyrics FILE] [--model 0.6b|1.7b]`**
  is spawned with posix_spawn exactly like `stems_demucs.py`. It prints line
  records on stdout, which the C++ side parses the same way it parses the
  stems protocol:
  ```
  progress  0.42
  phrase    <idx>  <t0>  <t1>  <avg_logprob|conf>  <text>
  word      <phrase_idx>  <t0>  <t1>  <word>          # optional, from aligner
  aligned   <line_idx>  <t0>  <t1>  <score>           # mode 2, see below
  aword     <line_idx>  <word_idx>  <t0>  <t1>
  status    done
  ```
- **Cache** in `~/.cache/beatmapper/lyrics/<stem-hash>/<model>.tsv`, keyed on
  the stem cache hash plus the model id. Add a hash of the lyric text for
  the aligned output.

### Phase A: transcription and line placement

1. **Phrases (C++ or script).** Run RMS-VAD on the vocal stem: frame RMS
   with a threshold relative to the track's vocal RMS distribution, 1 s
   minimum silence, merge to at most about 30 s, 0.2-0.3 s padding. Doing
   this in C++ lets the UI show phrase boxes before any ML runs, and lets us
   pass the boundaries to the script (`--segments`).
2. **ASR per phrase** gives `phrase` lines. Force the language (store it per
   track, defaulting to the language guessed from the lyric text). Drop
   phrases whose text is empty, very short, or flagged as repetitive.
3. **Fuzzy matching (C++).**
   - Normalize both sides: lowercase, strip punctuation, contractions to
     canonical form, numbers to words. Optionally map words to phonetic keys
     (Double Metaphone) so mishearings like "the ice of the tiger" still
     match.
   - Score each (phrase, line) pair by token-level Levenshtein or
     Smith-Waterman similarity over words, with character-level CER as a
     tiebreak.
   - Run a **monotonic DP** like DTW across phrases × lines, where:
     - a line can be skipped (not sung, or missed by ASR);
     - a phrase can be skipped (ad-lib, backing vocal, or hallucination);
     - a phrase can span two or more consecutive lines (common when the VAD
       merges);
     - a line may *repeat* (choruses). Let the DP jump back to any earlier
       block of lines with a penalty, or pre-expand the sheet using
       repeated-block detection.
   - The result is (line → phrase or sub-span) with a confidence. When a
     phrase covers several lines, split its time span using word timestamps
     if we have them, otherwise in proportion to character counts.
   - Write the results into `LyricMap` as `(t_start, t_end, text)`. Mark
     low-confidence lines in the UI for manual fixing; the lyric editing
     tools already exist.

### Phase B: word timing (the bonus)

4. For each placed line, send `(phrase audio ± 0.5 s, line text)` to
   Qwen3-ForcedAligner (mode 2: `--align PAIRS.tsv`), which gives `aword`
   records. Snap line `t_start` to the first word onset. Optionally snap word
   onsets to nearby beat or onset detections we already compute; sung word
   onsets usually land on the grid.
5. Alternatives for this step, behind the same interface:
   - ctc-forced-aligner/MMS with `<star>` edges (non-commercial weights);
   - schufo lyrics-aligner (English, MIT);
   - whisper.cpp DTW times when everything is native (coarser).

### Embedded-only variant (no new venv)

Link **whisper.cpp** as a static library:

- `ggml-large-v3` (or v2) at Q5_0, about 1.1 GB, or turbo for previews.
- Metal on; the Core ML encoder is optional.
- Feed 16 kHz mono phrases from the vocal stem; set `language`; set
  `no_context = true`; set `token_timestamps` / `dtw_token_timestamps`.
- Run on the existing worker-thread pattern.

Phase A is unchanged. Phase B uses the DTW times, or later an ONNX/Core ML
CTC acoustic model with our own Viterbi in C++ (with Core ML Qwen3-ForcedAligner as the more accurate alternative). This trades WER and timing
accuracy for zero Python.

### Evaluation

Add a small bench to `bench/` with these metrics:

- **Line-placement accuracy**: share of lines whose start is within 0.5 s of
  a hand-placed reference, using lyric maps already made by hand in the app.
- **Word mean/median absolute error** on a few tracks annotated at word
  level.
- **WER** of the phrase transcripts as a diagnostic.

Compare:

- Qwen3-ASR 0.6B vs 1.7B vs whisper.cpp large-v2/v3/turbo;
- vocal stem vs mix for ASR, keeping stem-based VAD in both cases;
- Qwen3-ForcedAligner vs MMS CTC for word timing.

Keep known-hard tracks (rap, heavy distortion, non-English) as a separate
group, like the beat and chord benches.
