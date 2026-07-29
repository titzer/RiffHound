#pragma once
#include <stdint.h>

// Decode any audio file ffmpeg understands to interleaved stereo f32.
//
// riffdsp shells out to ffmpeg rather than linking a decoder: it keeps the
// library dependency-free (libc + libm only), and it accepts every format in
// the library including .m4a, which miniaudio cannot decode on its own.
//
// Returns true on success; caller frees *out_pcm with free().
bool audio_decode(const char* path, float** out_pcm,
                  uint64_t* out_frames, uint32_t* out_channels,
                  uint32_t* out_sample_rate);
