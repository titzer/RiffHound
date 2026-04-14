#pragma once

#include "wsola.h"
#include "miniaudio.h"
#include <stdint.h>

// PitchNode: a ma_data_source that wraps WsolaSource and applies pitch
// shifting via ma_resampler (linear SRC).
//
// Pitch ratio P = 2^(total_cents / 1200), where total_cents = semitones*100 + cents.
//
// Pipeline:
//   WsolaSource [analysis hop = HOP*speed/P]
//       --> PitchNode [resampler ratio in/out = P]
//       --> engine
//
// The WSOLA analysis hop runs at speed/P so that, after the resampler
// compresses (P>1) or expands (P<1) the audio, the perceived tempo stays at S
// while the perceived pitch shifts by P.
//
// When P == 1.0 the resampler is bypassed entirely for efficiency.

// Max WSOLA frames buffered per read call.
// At P=2.0 (max pitch up = 12 semitones) the resampler consumes ~2× the output
// count.  8192 frames * 2 ch * 4 bytes = 64 KB as a struct member (static alloc).
#define PITCH_INBUF_FRAMES 8192

struct PitchNode {
    ma_data_source_base  base;           // must be first
    WsolaSource*         wsola;
    ma_resampler         resampler;
    float                current_pitch;  // pitch ratio last committed to resampler
    uint32_t             sample_rate;    // native sample rate (engine target)
    uint32_t             channels;
    float                inbuf[PITCH_INBUF_FRAMES * WSOLA_MAX_CH];
};

// Initialize. Must be called after wsola_init() succeeds.
// sample_rate and channels must match the WsolaSource.
bool pitch_node_init(PitchNode* pn, WsolaSource* ws,
                     uint32_t sample_rate, uint32_t channels);
void pitch_node_uninit(PitchNode* pn);
