#pragma once

#include "miniaudio.h"
#include <stdint.h>
#include <atomic>

// StretchSource: a miniaudio data source that plays a PCM buffer with
// independent speed (tempo) and pitch, built from two stages:
//
//   pcm --> phase vocoder, time-scaled by speed/pitch --> sinc resampler, by pitch
//
// The phase vocoder (STFT, Hann analysis + synthesis windows, 8x overlap)
// keeps the synthesis hop fixed and advances the analysis position by
// hop * speed / pitch.  Its phases come from identity phase locking (Laroche
// & Dolson 1999): each spectral peak's phase advances by its instantaneous
// frequency, and every bin in the peak's region of influence is rotated by
// the same angle, so a partial's main lobe stays coherent instead of smearing
// into the "phasey" sound of a bin-by-bin vocoder.  Both channels are rotated
// by angles derived from their sum, which keeps the stereo image stable.
// At drum hits and other attacks the bins carrying the new energy take the
// input's phases again (a transient phase reset), so attacks stay sharp.
//
// The windowed-sinc resampler then reads the stretched stream at rate
// `pitch`, which restores the tempo and shifts every frequency by `pitch`;
// it low-passes at the new Nyquist when pitching up, so nothing aliases.
//
// speed == pitch == 1 reproduces the input (up to float rounding).

#define STRETCH_MAX_CH 2

struct StretchState;   // audio-thread DSP state (stretch.cpp)

struct StretchSource {
    ma_data_source_base base;   // must be first member

    // Input PCM (interleaved f32, channels <= STRETCH_MAX_CH)
    float*   pcm;
    uint64_t frame_count;
    uint32_t channels;
    uint32_t sample_rate;
    bool     owns_pcm;      // if true, stretch_uninit frees pcm

    // Playback override: another buffer of exactly frame_count frames (same
    // layout) that plays in place of pcm -- a mix of selected stems.  The
    // main thread posts it in override_pending; the audio thread adopts it
    // at the start of a read callback and hands the previous one back in
    // override_retired for the main thread to free (stretch_collect_retired).
    // So pcm itself, which the analysis tools read, never changes.
    std::atomic<float*> override_pending;
    float*              override_active;    // audio thread only
    std::atomic<float*> override_retired;

    std::atomic<float>    speed;           // [0.25, 2.0]; set from main thread
    std::atomic<float>    pitch;           // frequency ratio; set from main thread
    std::atomic<uint64_t> cursor_frames;   // input frame now leaving the source (for UI)

    // Loop parameters (set from main thread, read from audio thread)
    std::atomic<bool>     loop_enabled;
    std::atomic<uint64_t> loop_start_frames;
    std::atomic<uint64_t> loop_end_frames;

    StretchState* st;
};

// Initialize with caller-owned (or borrowed) PCM data.
// If owns_pcm==true, stretch_uninit() frees the buffer with free().
bool  stretch_init(StretchSource* ss, float* pcm, uint64_t frames,
                   uint32_t channels, uint32_t sample_rate, bool owns_pcm);
void  stretch_uninit(StretchSource* ss);

// Post a playback override (frame_count frames, interleaved, takes
// ownership) or nullptr to go back to pcm.  Main thread.
void  stretch_set_override(StretchSource* ss, float* pcm);
// Free whatever the audio thread retired.  Call regularly from the main
// thread (audio_update does).
void  stretch_collect_retired(StretchSource* ss);

// Thread-safe speed / pitch accessors; picked up at the next analysis hop.
// pitch is a frequency ratio: 2^(total_cents/1200), clamped to [0.25, 4].
void  stretch_set_speed(StretchSource* ss, float speed);
float stretch_get_speed(const StretchSource* ss);
void  stretch_set_pitch(StretchSource* ss, float pitch);
float stretch_get_pitch(const StretchSource* ss);

// Thread-safe loop control (all parameters written atomically from main thread).
// loop_end_frames == 0 disables looping regardless of loop_enabled.
void  stretch_set_loop(StretchSource* ss, bool enabled,
                       uint64_t loop_start_frames, uint64_t loop_end_frames);
