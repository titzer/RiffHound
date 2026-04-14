#pragma once

#include "miniaudio.h"
#include <stdint.h>
#include <atomic>

// WSOLA (Waveform Similarity Overlap-Add) custom miniaudio data source.
// Provides pitch-preserving time stretching by choosing each analysis frame
// to maximize waveform continuity with the previous synthesis output.
//
// Parameters (75% overlap, ~2x analysis/synthesis frame ratio):
#define WSOLA_FRAME    2048   // analysis/synthesis frame size (samples)
#define WSOLA_HOP       512   // synthesis hop size (samples per WSOLA step)
#define WSOLA_SEARCH    128   // ±sample search radius for best analysis frame
#define WSOLA_MAX_CH      2   // maximum supported channel count

struct WsolaSource {
    ma_data_source_base base;   // must be first member

    // Input PCM (interleaved f32, channels <= WSOLA_MAX_CH)
    float*   pcm;
    uint64_t frame_count;
    uint32_t channels;
    uint32_t sample_rate;
    bool     owns_pcm;      // if true, wsola_uninit frees pcm

    // Playback state
    // input_pos written by audio thread; cursor_frames readable from any thread
    double                input_pos;       // fractional input frame index
    std::atomic<float>    speed;           // [0.25, 2.0]; set from main thread
    std::atomic<float>    pitch;           // [0.5, 2.0] ratio; set from main thread
    std::atomic<uint64_t> cursor_frames;   // last committed input frame (for UI)

    // Loop parameters (set from main thread, read from audio thread)
    std::atomic<bool>     loop_enabled;
    std::atomic<uint64_t> loop_start_frames;
    std::atomic<uint64_t> loop_end_frames;

    // WSOLA synthesis accumulation buffer (overlap-add staging)
    float synth_buf[WSOLA_FRAME * WSOLA_MAX_CH];
    // One hop of output ready to drain before the next wsola_step
    float output_buf[WSOLA_HOP  * WSOLA_MAX_CH];
    int   output_pending;   // frames staged in output_buf
    int   output_offset;    // frames already consumed from output_buf
    bool  first_frame;      // skip search on very first step
};

// Initialize with caller-owned (or borrowed) PCM data.
// If owns_pcm==true, wsola_uninit() frees the buffer with free().
bool  wsola_init(WsolaSource* ws, float* pcm, uint64_t frames,
                 uint32_t channels, uint32_t sample_rate, bool owns_pcm);

void  wsola_uninit(WsolaSource* ws);

// Thread-safe speed accessors (atomic).
void  wsola_set_speed(WsolaSource* ws, float speed);
float wsola_get_speed(const WsolaSource* ws);

// Thread-safe pitch accessors (atomic).
// pitch is a frequency ratio: 2^(total_cents/1200).  [0.5, 2.0] = ±12 semitones.
// The PitchNode reads this same atomic to update its resampler on the audio thread.
void  wsola_set_pitch(WsolaSource* ws, float pitch);
float wsola_get_pitch(const WsolaSource* ws);

// Thread-safe loop control (all parameters written atomically from main thread).
// loop_end_frames == 0 disables looping regardless of loop_enabled.
void  wsola_set_loop(WsolaSource* ws, bool enabled,
                     uint64_t loop_start_frames, uint64_t loop_end_frames);
