# Suno and other generative models for parody, looper and live-jam use cases

Research date: 2026-10-15. Scope: what Suno (and its alternatives) let an
application do programmatically, judged against three RiffHound use cases:

1. **Parody.** Take a library song, beatmap and stem-separate it, transcribe
   the vocal melody, then sing new lyrics on that melody over the original
   backing.
2. **Jam looper.** Loop parts of an existing song and have a model add bass,
   drums and other parts.
3. **Live jam partner.** Follow live guitar (tempo, rhythm, riffs) in real
   time, fill in missing parts and riff back.

Sources are listed at the end. Many details about Suno come from third-party
blogs rather than Suno's own docs. Where a claim rests on such a source, the
text says so.

## 1. Recommendation

- **Suno has no public API.** You cannot build on it today in any
  supported way (section 2). The only legitimate path is the partner intake
  form opened on 2026-07-01, and acceptance is selective. Third-party "Suno
  APIs" are reverse-engineered wrappers. They break Suno's ToS, risk account
  bans, and break when Suno changes. Also, on 2026-09-09 Suno retired every
  model before V6, which broke the wrappers' documented model lists.
- **Even with API access, Suno fits use case 1 poorly and use cases 2–3 not
  at all.** It blocks uploads of copyrighted recordings, which is exactly
  what parodying a library song needs. It works at the scale of whole songs
  and takes tens of seconds per job. And its useful editing features (Studio
  2.0 MIDI-to-audio covers, stem generation) exist only inside its browser
  DAW.
- **Build on local open models instead.** They fit RiffHound's existing
  sidecar pattern (Python/MLX next to the C++ app, as with Demucs):
  - **Use case 1:** **SoulX-Singer** (Apache-2.0). It sings
    lyrics on a MIDI score or F0 contour and clones timbre zero-shot. Its
    preprocessing already includes vocal separation, RMVPE pitch, ASR and
    note transcription. The hard part RiffHound must own is fitting new
    lyric syllables onto the existing notes.
  - **Use case 2:** **ACE-Step 1.5**. Its `lego` task adds an instrument layer
    to existing audio, `complete` adds backing to a vocal, `repaint`
    regenerates a time range, and `extract` pulls a stem. Generate offline
    a few bars ahead, then lock the result to the beat grid with the
    existing stretch engine.
  - **Use case 3:** **Magenta RealTime 2** (open weights, MLX, C++
    engine, ~200 ms control latency, MIDI-pianoroll conditioning). Combine it
    with a **symbolic layer** (detected chords → MIDI bass and drums,
    played on a synth or samples) for the parts that must be sample-tight.
    No cloud model, Suno included, can follow a live player.
- **Cloud fallbacks with real APIs:** ElevenLabs Music v2.5 (composition plans,
  inpainting, ≤30 s style reference) and Google Lyria (`lyria-3.5` for songs,
  `lyria-realtime-exp` for streaming instrumental). Neither accepts a melody
  to sing, and neither will cover or edit an uploaded song. They suit
  "generate a backing track in style X at tempo Y". They don't suit
  "accompany *this* audio".

## 2. Suno as a platform (state as of October 2026)

### 2.1 API status

- **No public API.** Suno has no developer console, no API keys, no
  endpoint reference and no OpenAPI spec.
- **Partner program.** On 2026-07-01, CPO Jack Brody posted that Suno is
  "exploring a developer API" for a "curated group of partners". He
  linked an intake form (`sunomusic.typeform.com/apiform`) and said Suno
  wants apps that "unlock experiences generative music makes possible for the
  first time". No timeline, capability list or licensing terms have been
  published. MBW's description: outside apps "submit text prompts …
  and receive finished audio".
- **Terms of service.** The ToS bans data mining, robots, scraping, and
  getting content "through any means not intentionally made available". So
  the cookie-and-captcha wrappers (gcui-art/suno-api, and hosted resellers
  such as sunoapi.org, AceData, aimusicapi) break the ToS. Hosted
  resellers also pool accounts, so the rights to what you generate are
  unclear.

**Takeaway:** a RiffHound application could apply to the partner form,
probably with use case 1 or 3 as the pitch, since it is novel and
"first time" fits. But don't put Suno on the critical path.

### 2.2 What the product can do (useful for knowing what to ask for)

The reverse-engineered wrappers show what the web app does internally.
These are the operations an official API would likely expose:

| Operation | Notes |
|---|---|
| Generate (prompt + lyrics + style) | Up to ~8 min. Vocal or instrumental. |
| Extend | Continue a song from a timestamp. |
| Upload and cover / upload and extend | Restyle or continue *your* audio. Upload is fingerprinted, and copyrighted recordings are refused (or silently muted, per a third-party report). |
| Add vocals / add instrumental | Sing over an uploaded instrumental, or back an uploaded vocal. |
| Separate vocals / stems | Up to 12 stems on Premier (per blogs). |
| Timestamped lyrics | Word timings for generated songs. |
| Personas / voices | Reusable voice identity. "Record or upload your own voice" was added in 5.5 (paid). |

**V6 (launched 2026-09-09, developed "with our industry partners, including
Warner Music Group, BMG and Believe")** adds the following, per third-party
reviews:

- **Section editing:** regenerate a section from a plain-language
  instruction.
- **Word- and line-level lyric edits** without regenerating the track.
- **Mashups** of your own songs.
- **Riff sampling:** isolate a riff and build a beat around it.
- **Multimodal starting points:** audio, image or video.

All earlier models (v4.5 to v5.5) were retired the same day. Extend, remix
and cover on old songs now run on V6.

**Studio 2.0 (2026-08-13)**, the browser DAW, adds:

- MIDI import, recording and editing, with a wavetable synth.
- **"MIDI clip → audio cover"** of those notes.
- **Audio → MIDI** ("Get MIDI", ~10 credits per stem).
- Uncapped 32-bit/48 kHz stem export on Premier.

This is the closest Suno comes to use case 1 (a melody in, a sung or
played rendering out), but it exists only in the UI.

**Download caps (from 2026-09-03, retroactive):**

- Free: 7 lifetime downloads, non-commercial use only.
- Pro: 20 per month.
- Premier: 60 per month.
- One download covers a song and its stems. Extra downloads cost ~$3 each
  (unconfirmed).

Iterating on many variations, as a looper would, burns through these caps
quickly.

**Legal climate:** Warner settled in November 2025. UMG and Sony are still
in litigation, and one unconfirmed report says they filed a second suit on
2026-09-18 aimed at V6. Udio is worse for app builders: since the UMG deal
it has been a walled garden with no audio or stem export, and it has no
API.

### 2.3 Suno against each use case

| Use case | Fit | Why |
|---|---|---|
| 1. Parody | Poor | Uploading the original song or its vocal stem is blocked by fingerprinting. An instrumental stem may also match. A synth rendering of the transcribed melody would probably pass, and "add vocals" or a Studio MIDI cover with new lyrics is conceptually the right operation. But Suno does not promise to follow your notes exactly, and it is UI-only or ToS-breaking. |
| 2. Looper | Poor | Generation takes tens of seconds and works on whole songs. Tempo and bar alignment to your loop is not guaranteed. Download caps hit hard. Section and stem features exist only in Studio. |
| 3. Live jam | None | There is no streaming mode, and latency is in seconds to minutes. |

## 3. Use case 1: parody pipeline

```
library song ──beatmapper──▶ beat grid, sections, key/chords
     │
     └─Demucs htdemucs_6s──▶ vocals.wav + instrumental (other 5 stems)
                                  │
       vocal melody transcription ┘  (RMVPE F0 → note segmentation, e.g. ROSVOT)
                                  │
       original lyrics + word timings (existing lyric aligner) ──▶ syllable↔note map
                                  │
       new lyrics (user or LLM, constrained to syllables/stress per phrase)
                                  │
       re-map syllables onto notes (split/merge notes where counts differ)
                                  │
       SoulX-Singer SVS (score/MIDI mode, timbre prompt = original or another voice)
                                  │
       optional SoulX-Singer-SVC / Seed-VC to push timbre closer
                                  │
       mix over instrumental stem, align to beat grid ──▶ parody.wav
```

- **Singing model.** SoulX-Singer (Feb 2026, Apache-2.0) is
  flow-matching with a DiT decoder. It trains on 42k+ hours of Mandarin,
  English and Cantonese. Its conditioning modes are **melody (F0)** and
  **score (MIDI)**, with a zero-shot timbre prompt. Its paper reports
  lower F0 error than YingMusic-Singer in melody mode.
- **Its preprocessing matches our pipeline.** It runs vocal separation,
  dereverberation, RMVPE, Paraformer/Parakeet ASR and ROSVOT note
  transcription. It also warns that automatic lyric/note alignment needs
  manual correction, and ships a MIDI editor for that.
  - RiffHound can do this step better than their script, because we
    already have aligned lyrics (see `lyrics-transcription.md`) and a beat
    grid.
- **Alternatives.** YingMusic-Singer (diffusion, annotation-free melody
  guidance, aimed at lyric replacement) is the closest competitor, and its
  "flexible lyric manipulation" framing matches parody directly. Benchmark
  both. DiffSinger is mature but needs a per-voice model, so it can't clone
  a voice zero-shot.
- **F0 mode or score mode.** F0 mode keeps the original singer's slides,
  vibrato and phrasing, which helps the parody sound like the original
  performance. But the F0 contour carries the original syllable timing,
  so it fits only when the new lyrics keep the same syllable count per
  phrase. Score mode is more forgiving when syllable counts differ. Use
  F0 mode by default and fall back to score mode for re-timed phrases.
- **The hard part is lyric fitting, not synthesis.** Rules for the
  syllable↔note mapper:
  - Keep the syllable count per phrase.
  - Put stressed syllables on strong beats. The beat grid supplies the
    metrical position.
  - Allow melisma (one syllable over several notes) or note splits when
    counts differ by ±1–2.
  - An LLM can write lyrics against a per-phrase template such as
    `da-DUM-da-da-DUM (5 syl, stress 2,5)`. A small checker (CMUdict
    syllables and stress) can verify the result and send failures back
    to the LLM.
- **Rights.** Parody of a copyrighted song is judged case by case under fair
  use. For personal use inside the app, this is the user's call. Don't
  route the original audio to cloud services that fingerprint it.

## 4. Use case 2: jam looper with generated parts

- **Loop source.** Use beatmapper's bars and sections to cut loops on bar
  boundaries, then pick the stems to keep (for example, drop the bass
  stem and loop the rest).
- **Generation: ACE-Step 1.5 (local, open).**
  - `lego` adds a layer (bass under a guitar loop, drums under a
    riff).
  - `complete` generates backing for a vocal.
  - `repaint` regenerates a range.
  - `cover` restyles, with `audio_cover_strength` from 0 to 1.
  - `extract` pulls one track out.
  - `lego`, `complete` and `extract` need the **base or SFT** model
    (32–50 steps), not turbo.
  - Speed: a 30 s clip takes ~3–8 s on a 12 GB GPU. Expect several times
    that on MPS. An Apple-Silicon port exists (MPS/MLX).
  - LoRA training on 8–20 songs gives a personal style.
- **Lock to the grid.** Generated layers drift. Beat-track each one,
  then warp it onto the loop's grid with the PVDR stretch engine, and
  trim or crossfade at the loop boundary. Generate 2–4 candidates per
  layer in the background and let the user flip between them, as a
  looper's "variation" button.
- **Cheaper, tighter alternative for bass and drums: symbolic.**
  Beatmapper already has chords and a beat grid. Write a MIDI bass line
  (root/fifth/approach notes, or a small model) and a drum pattern, then
  render them with a sampler. This gives a perfect grid, instant results
  and the option to edit the notes. Use generative audio where *timbre and
  feel* matter; use symbolic where *tightness* matters.
- **Cloud options if wanted.** ElevenLabs Music v2.5 composition plans
  (≤30 chunks, per-section style and duration) can generate a backing
  track to a tempo and style. Its Audio Reference (≤30 s, screened for
  copyright) only guides style, so it can't add parts to your loop.
  Stable Audio 2.5 does audio inpainting (regenerates a
  `[mask_start, mask_end]` window, ~$0.20 per clip on fal) and
  audio-to-audio. That is the nearest cloud equivalent of "continue or
  fill my loop".

## 5. Use case 3: real-time jam partner

No generative audio model "listens" to a player yet. The Lyria team's
*Live Music Models* paper calls true jamming future work. MRT2 also lists
real-time audio input as future work. So the system has to be
**analysis → control → generation**:

```
guitar in ─▶ onset/beat tracker (causal) ─▶ tempo + phase, bar clock
         ├─▶ pitch/chord detector       ─▶ current chord, key, riff notes (MIDI)
         └─▶ riff memory (bar-quantised phrases)

bar clock + chords ─▶ symbolic layer: bass + drums MIDI → sampler   (tight, ~0 latency, scheduled ahead)
                   └▶ MRT2: pianoroll = chord tones / riff notes,
                        style = text + audio embedding of your tone  (texture, pads, keys; ~200 ms)
riff memory ─▶ call-and-response: transpose/vary phrase symbolically, play back next bar(s)
```

- **Magenta RealTime 2** (preview, June 2026):
  - Two sizes: 2.4B base and 230M small. Weights CC-BY-4.0, code
    Apache-2.0.
  - It generates 40 ms frames, with **~200 ms control latency** (MRT1
    was ~3 s).
  - The **MLX C++ engine** handles state, buffering, resampling and MIDI
    input, and can be embedded in a host app.
  - Real time with the base model needs an M3 Pro / M2 Max or better
    (the M4 Pro qualifies). The small model runs in real time on any
    Apple Silicon Mac.
  - **MIDI conditioning** takes a 128-pitch pianoroll. "Auto-Strum" lets
    the model place onsets; with it off, you set onsets yourself. Drums
    are only on/off.
  - Style prompts come from text or audio through MusicCoCa embeddings.
  - Tempo sync is not documented. You'd probably need to steer it
    through rhythmic MIDI onsets and/or warp its output.
- **Lyria RealTime** (`lyria-realtime-exp`, WebSocket, 48 kHz stereo PCM)
  is the cloud equivalent:
  - It has `bpm` (60–200), `scale`, `density`, `brightness`,
    `mute_bass`, `mute_drums` and `only_bass_and_drums`.
  - It is instrumental only, takes **no audio input**, and is watermarked.
  - Changing BPM or scale needs `reset_context()`, which is too abrupt
    for following a drifting human tempo.
  - It's handy for a prototype, but network jitter makes it worse than
    MRT2 locally.
- **Hide latency with musical anticipation, not speed.** A bandmate
  doesn't react within 10 ms either. They commit to the next bar from
  what they've heard. Schedule the symbolic parts one beat to one bar
  ahead, from the predicted chord (often a repeat of the progression). Then:
  - Feed MRT2's pianoroll the chord *for the upcoming bar*, so its
    ~200 ms lag lands on the downbeat.
  - Treat "riffing back" as call-and-response: capture a 1–2 bar
    phrase, then play a variation (transposed, re-rhythmed, or
    re-voiced through MRT2's pianoroll) in the next gap.
- **Research systems worth reading:**
  - ReaLJam (CHI 2025): an RL-tuned transformer plays chords along with
    a user melody, with anticipation built in.
  - StreamMUSE (arXiv 2606.11886, RTAS 2026, open source):
    frame-synchronous streaming LM accompaniment, plus a latency and
    hyperparameter model.
  - JazzyGPT2: a GPT-2 that generates MIDI accompaniment on-device.
  - MazzikaAI (2026): steers Lyria RealTime from live MIDI, by turning
    the performance into prompts.
- **Where Suno and ACE-Step fit.** Not in the live loop. They can
  pre-generate "band" material in the background (for example, a groove
  library in the user's style) that the live system cues and warps to
  tempo.

## 6. Model and service comparison

| | Access | Melody/notes in | Audio in (accompany it) | Vocals | Latency | Local on M4 Pro |
|---|---|---|---|---|---|---|
| Suno V6 | No public API (partner form only) | Studio UI only (MIDI cover) | Upload cover/add-vocals; copyrighted audio blocked | Yes | 10 s–min | No |
| ElevenLabs Music v2.5 | REST, paid plans | No | Style ref ≤30 s only | Yes | seconds | No |
| Lyria 3.5 / 3 Clip | Gemini API | No (images + text only) | No | Yes | seconds | No |
| Lyria RealTime | Gemini WebSocket, experimental | No | No | Instrumental | streaming | No |
| Stable Audio 2.5 | Stability / fal / Replicate | No | Inpaint, audio-to-audio | Limited | seconds | No |
| ACE-Step 1.5 | Open weights | Via cover strength | lego / complete / repaint / extract | Yes | ~seconds per 30 s (GPU) | Yes (MPS/MLX port) |
| Magenta RT 2 | Open weights, MLX C++ | Pianoroll MIDI | Style embedding only | No | ~200 ms | Yes |
| SoulX-Singer | Open, Apache-2.0 | MIDI score or F0 | SVC variant (voice → voice) | Yes (singing only) | offline | Likely (untested) |

## 7. Suggested next steps

1. **Parody spike.** Install SoulX-Singer in a venv next to the Demucs one.
   Feed it one phrase: Demucs vocal stem → RMVPE F0 → hand-written new
   lyric with the same syllable count → F0-mode synthesis, then mix over
   the instrumental. This tests quality and MPS speed before building the
   lyric mapper.
2. **MRT2 spike.** Build the MLX C++ engine and drive its pianoroll from
   beatmapper's chord track while playing a song. Measure the real
   end-to-end latency and see whether its rhythm can be entrained by MIDI
   onsets.
3. **ACE-Step `lego` spike.** Use one 4-bar guitar loop and generate a
   bass layer (base model). Check how well it keeps tempo and key before
   warping.
4. **Optional.** Submit the Suno partner form, describing the
   parody/jam-partner app, in case API access opens up with
   melody-conditioned generation.

## Sources

- Suno API exploration: [MBW, 2026-07-02](https://www.musicbusinessworldwide.com/suno-explores-developer-api-seeking-apps-that-unlock-experiences-generative-music-makes-possible-for-the-first-time/), [Tunova guide](https://tunova.ai/guides/is-there-an-official-suno-api), [AIMLAPI on unofficial wrappers](https://aimlapi.com/blog/the-suno-api-reality), [Sonilo SaaS guide](https://sonilo.com/blog/guides/official-suno-api-saas-builders-2026)
- Suno terms: [suno.com/legal/terms](https://suno.com/legal/terms), [ConductAtlas scraping clause](https://conductatlas.com/platform/suno/suno-terms-of-service/prohibition-on-data-mining-and-scraping/)
- Suno V6 and caps: [Played.fm V6 review](https://played.fm/blog/suno-v6-review), [Undetectr on licensed models](https://undetectr.com/blog/suno-licensed-models-2026), [Choosely v6 vs v5.5](https://choosely.ai/ai-radar/suno-v6-vs-v5-5-what-changed)
- Suno Studio 2.0: [Dubspot](https://blog.dubspot.com/suno-studio-2-0), [Suno help: exporting from Studio](https://help.suno.com/en/articles/8128193), [Jack Righteous guide](https://jackrighteous.com/en-us/blogs/guides-using-suno-ai-music-creation/suno-studio-v5-complete-guide)
- Suno uploads: [Suno blog: audio inputs](https://suno.com/blog/audio-inputs), [sunoapi.org docs (unofficial)](https://docs.sunoapi.org/)
- Warner deal: [MBW](https://www.musicbusinessworldwide.com/warner-music-group-settles-with-suno-strikes-first-of-its-kind-deal-with-ai-song-generator/), [Digital Music News](https://www.digitalmusicnews.com/2025/12/22/suno-warner-music-deal-changes/)
- Udio: [Digital Music News](https://www.digitalmusicnews.com/2025/11/03/umg-udio-vexing-questions-swirl/), [Sonilo](https://sonilo.com/blog/comparisons/udio-v4-warner-music-licensing-explained)
- ElevenLabs: [Music capabilities](https://elevenlabs.io/docs/overview/capabilities/music), [Inpainting guide](https://elevenlabs.io/docs/eleven-api/guides/how-to/music/inpainting), [API reference (skills repo)](https://github.com/elevenlabs/skills/blob/main/music/references/api_reference.md)
- Google: [Lyria music generation](https://ai.google.dev/gemini-api/docs/music-generation), [Lyria RealTime API](https://ai.google.dev/gemini-api/docs/realtime-music-generation), [Lyria RealTime page](https://deepmind.google/models/lyria/lyria-realtime/), [Live Music Models paper](https://arxiv.org/pdf/2508.04651), [Magenta RealTime 2](https://magenta.withgoogle.com/magenta-realtime-2), [MRT2 on HF](https://huggingface.co/google/magenta-realtime-2)
- Stable Audio 2.5: [ComfyUI docs](https://docs.comfy.org/tutorials/api-nodes/stability-ai/stable-audio), [fal inpaint](https://fal.ai/models/fal-ai/stable-audio-25/inpaint/llms.txt)
- ACE-Step 1.5: [Musician's guide](https://github.com/ace-step/ACE-Step-1.5/blob/main/docs/en/ace_step_musicians_guide.md), [Tutorial](https://github.com/ace-step/ACE-Step-1.5/blob/main/docs/en/Tutorial.md), [Apple Silicon port](https://github.com/clockworksquirrel/ace-step-apple-silicon)
- Singing synthesis: [SoulX-Singer repo](https://github.com/Soul-AILab/SoulX-Singer), [paper](https://arxiv.org/pdf/2602.07803), [YingMusic-Singer](https://arxiv.org/pdf/2512.04779)
- Real-time accompaniment research: [ReaLJam](https://arxiv.org/pdf/2502.21267), [StreamMUSE / RT LM jamming](https://arxiv.org/abs/2606.11886), [Edge symbolic accompaniment (JazzyGPT2)](https://link.springer.com/chapter/10.1007/978-3-032-05748-8_44), [MazzikaAI](https://arxiv.org/html/2608.10360v1)
