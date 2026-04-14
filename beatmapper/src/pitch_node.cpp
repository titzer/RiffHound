#include "pitch_node.h"
#include <math.h>
#include <string.h>

// ---- vtable forward declarations -------------------------------------------

static ma_result pitch_node_on_read(ma_data_source* pDS, void* pFramesOut,
                                     ma_uint64 frameCount, ma_uint64* pFramesRead);
static ma_result pitch_node_on_seek(ma_data_source* pDS, ma_uint64 frameIndex);
static ma_result pitch_node_on_get_data_format(ma_data_source* pDS, ma_format* pFormat,
                                                ma_uint32* pChannels, ma_uint32* pSampleRate,
                                                ma_channel* pChannelMap, size_t channelMapCap);
static ma_result pitch_node_on_get_cursor(ma_data_source* pDS, ma_uint64* pCursor);
static ma_result pitch_node_on_get_length(ma_data_source* pDS, ma_uint64* pLength);

static ma_data_source_vtable s_pitch_vtable = {
    pitch_node_on_read,
    pitch_node_on_seek,
    pitch_node_on_get_data_format,
    pitch_node_on_get_cursor,
    pitch_node_on_get_length,
    NULL,   // onSetLooping
    0,      // flags
};

// ---- helpers ----------------------------------------------------------------

// (Re)initialize *r as a linear resampler with in/out ratio = pitch.
// sampleRateIn = round(sr * pitch), sampleRateOut = sr.
// Ratio > 1 (pitch up) => resampler compresses input => higher pitch.
static bool init_resampler(ma_resampler* r, uint32_t ch, uint32_t sr, float pitch)
{
    ma_uint32 sr_in = (ma_uint32)(sr * pitch + 0.5f);
    if (sr_in == 0) sr_in = sr;
    ma_resampler_config cfg = ma_resampler_config_init(
        ma_format_f32, ch, sr_in, sr, ma_resample_algorithm_linear);
    return ma_resampler_init(&cfg, NULL, r) == MA_SUCCESS;
}

// ---- vtable implementations -------------------------------------------------

static ma_result pitch_node_on_read(ma_data_source* pDS, void* pFramesOut,
                                     ma_uint64 frameCount, ma_uint64* pFramesRead)
{
    PitchNode* pn = (PitchNode*)pDS;
    float pitch = pn->wsola->pitch.load(std::memory_order_relaxed);

    // Fast path: no pitch shift — bypass the resampler entirely.
    // Check this BEFORE touching the resampler to avoid set_rate_ratio(1.0).
    if (fabsf(pitch - 1.0f) < 1e-6f) {
        return ma_data_source_read_pcm_frames(pn->wsola, pFramesOut, frameCount, pFramesRead);
    }

    // Pick up any pitch change written by the main thread (non-1.0 only).
    if (pitch != pn->current_pitch) {
        ma_resampler_set_rate_ratio(&pn->resampler, pitch);
        pn->current_pitch = pitch;
    }

    // Determine how many WSOLA frames are needed to produce frameCount output frames.
    ma_uint64 in_needed = 0;
    ma_resampler_get_required_input_frame_count(&pn->resampler, frameCount, &in_needed);
    in_needed += 4;   // small slack for resampler rounding
    if (in_needed > PITCH_INBUF_FRAMES) in_needed = PITCH_INBUF_FRAMES;

    // Pull from WSOLA into the intermediate buffer.
    ma_uint64 wsola_read = 0;
    ma_result wsola_res = ma_data_source_read_pcm_frames(
        pn->wsola, pn->inbuf, in_needed, &wsola_read);

    // Push through the resampler.
    ma_uint64 frames_in  = wsola_read;
    ma_uint64 frames_out = frameCount;
    ma_resampler_process_pcm_frames(&pn->resampler,
                                    pn->inbuf, &frames_in,
                                    pFramesOut, &frames_out);

    if (pFramesRead) *pFramesRead = frames_out;
    // Signal AT_END only when WSOLA is exhausted and we couldn't fill the buffer.
    return (wsola_res == MA_AT_END && frames_out < frameCount) ? MA_AT_END : MA_SUCCESS;
}

static ma_result pitch_node_on_seek(ma_data_source* pDS, ma_uint64 frameIndex)
{
    PitchNode* pn = (PitchNode*)pDS;
    // Rebuild the resampler to flush any stale interpolation state.
    ma_resampler_uninit(&pn->resampler, NULL);
    init_resampler(&pn->resampler, pn->channels, pn->sample_rate, pn->current_pitch);
    return ma_data_source_seek_to_pcm_frame(pn->wsola, frameIndex);
}

static ma_result pitch_node_on_get_data_format(ma_data_source* pDS, ma_format* pFormat,
                                                ma_uint32* pChannels, ma_uint32* pSampleRate,
                                                ma_channel* pChannelMap, size_t channelMapCap)
{
    PitchNode* pn = (PitchNode*)pDS;
    if (pFormat)     *pFormat     = ma_format_f32;
    if (pChannels)   *pChannels   = pn->channels;
    if (pSampleRate) *pSampleRate = pn->sample_rate;
    if (pChannelMap)
        ma_channel_map_init_standard(ma_standard_channel_map_default,
                                     pChannelMap, channelMapCap, pn->channels);
    return MA_SUCCESS;
}

static ma_result pitch_node_on_get_cursor(ma_data_source* pDS, ma_uint64* pCursor)
{
    PitchNode* pn = (PitchNode*)pDS;
    return ma_data_source_get_cursor_in_pcm_frames(pn->wsola, pCursor);
}

static ma_result pitch_node_on_get_length(ma_data_source* pDS, ma_uint64* pLength)
{
    PitchNode* pn = (PitchNode*)pDS;
    return ma_data_source_get_length_in_pcm_frames(pn->wsola, pLength);
}

// ---- public API -------------------------------------------------------------

bool pitch_node_init(PitchNode* pn, WsolaSource* ws,
                     uint32_t sample_rate, uint32_t channels)
{
    memset(&pn->base, 0, sizeof(pn->base));
    pn->wsola         = ws;
    pn->sample_rate   = sample_rate;
    pn->channels      = (channels > WSOLA_MAX_CH) ? WSOLA_MAX_CH : channels;
    pn->current_pitch = ws->pitch.load(std::memory_order_relaxed);

    ma_data_source_config dsc = ma_data_source_config_init();
    dsc.vtable = &s_pitch_vtable;
    if (ma_data_source_init(&dsc, &pn->base) != MA_SUCCESS)
        return false;

    if (!init_resampler(&pn->resampler, pn->channels, sample_rate, pn->current_pitch)) {
        ma_data_source_uninit(&pn->base);
        return false;
    }
    return true;
}

void pitch_node_uninit(PitchNode* pn)
{
    ma_resampler_uninit(&pn->resampler, NULL);
    ma_data_source_uninit(&pn->base);
}
