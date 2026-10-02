#pragma once

#include "spectrogram.h"
#include <stdint.h>

struct AudioState;

// Stem separation: the loaded track split into vocals / drums / bass /
// guitar / piano / other by Demucs (scripts/stems_demucs.py, run out of
// process), each stem decoded and given a spectrogram of its own.
//
// Everything lives in one module-level set.  Separation runs on a worker
// thread that spawns the script, follows its progress, then decodes the
// stem files and builds the spectrogram pixels; stems_update() on the GL
// thread lands the result and uploads the textures.
//
// Selection: any subset of stems, or the mix.  With stems selected, the
// timeline shows their spectrograms summed, each in its own colour, and
// playback is their audio summed (the mix's own buffer stays for the
// analysis tools).  Both are rebuilt by a worker on every change.

#define STEMS_MAX 8

enum StemsStatus {
    STEMS_IDLE = 0,    // nothing known for this track
    STEMS_CHECKING,    // asking the cache (fast, no model)
    STEMS_RUNNING,     // model running (progress in stems_progress())
    STEMS_READY,       // stems loaded
    STEMS_FAILED,      // script failed or not installed (stems_message())
};

void stems_init(AudioState* audio);
void stems_shutdown();

// Forget the current stems and stop any run (the track changed).
void stems_reset();

// Start a separation of `audio_path` in the background.  With cached_only
// the script answers only from its cache (used on track load so stems
// separated earlier come back without a click).  No-op while one runs.
void stems_request(const char* audio_path, bool cached_only);

// Once per frame on the GL thread: land finished work, upload textures,
// hand a rebuilt stem mix to playback.
void stems_update();

StemsStatus stems_status();
float       stems_progress();       // 0..1 while STEMS_RUNNING
const char* stems_message();        // human-readable status line
bool        stems_available();      // the separator venv + script exist

int               stems_count();
const char*       stems_name(int i);
SpectrogramState* stems_spectrogram(int i);
// Colour a stem is drawn in (by name, so an instrument always matches).
void              stems_tint(const char* name, float* rgb);

// --- selection -----------------------------------------------------------------
bool     stems_mix_selected();         // the mix: no stem selected
bool     stems_is_selected(int i);
uint32_t stems_selection_mask();
void     stems_toggle(int i);          // flip one stem; none left = the mix
void     stems_select_mix();           // clear the selection
void     stems_select_only(int i);
// Select these stems (comma-separated names) as soon as stems are loaded;
// for the --stems launch flag.
void     stems_select_names(const char* csv);

// The summed, per-stem-coloured spectrogram of the selection; nullptr when
// the mix is selected or it is still being built (show the mix meanwhile).
SpectrogramState* stems_composite();
bool              stems_rebuilding();  // selection work in flight

// --- analysis sources ------------------------------------------------------
// What a tool analyses: the mix (mask 0) or a sum of stems.  Each tool has a
// preset naming the stems that suit its job; with the track separated, the
// tool starts from that preset, and a user choice sticks until the stems
// change (another track).  The summed audio is mono f32 at the track's rate,
// decoded on first use and kept until the stems are dropped, so the pointer
// stays valid for a worker that holds it (see stems_reset).
enum StemPreset {
    STEM_SRC_MIX = 0,
    STEM_SRC_HARMONIC,   // guitar + piano + other + bass: chords, chroma
    STEM_SRC_RHYTHM,     // drums + bass: beats
    STEM_SRC_DRUMS,      // drums: onset timbre
};
struct StemSource {
    uint32_t mask   = 0;       // valid when gen == stems_generation()
    bool     custom = false;   // the user chose; else follows the preset
    int      gen    = -1;
};
int      stems_generation();                       // bumps when stems land or drop
uint32_t stems_preset_mask(StemPreset preset);     // 0 when the stems are missing
// The effective mask for a source (applies the preset on a new stem set).
uint32_t stems_source_mask(StemSource* src, StemPreset preset);
// The audio for a mask: the mix for 0 (audio_pcm_data), else the stem sum.
const float* stems_source_audio(uint32_t mask, uint64_t* frames,
                                uint32_t* channels, uint32_t* sample_rate);
// "mix" or "guitar+piano+other" for a mask.
void     stems_source_label(uint32_t mask, char* out, int out_size);
