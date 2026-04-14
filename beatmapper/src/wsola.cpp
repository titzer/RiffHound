#include "wsola.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <float.h>

// Hann window, amplitude-normalized for 75% overlap (FRAME/HOP = 4).
// With 75% overlap the sum of four overlapping standard Hann windows equals 2,
// so we pre-scale by 0.5 so the OLA sum is 1.0.
static inline float hann(int i, int N)
{
    return 0.25f * (1.0f - cosf(2.0f * 3.14159265f * i / N));
}

// ---- vtable callbacks -------------------------------------------------------

static ma_result wsola_on_read(ma_data_source* pDS, void* pFramesOut,
                                ma_uint64 frameCount, ma_uint64* pFramesRead);
static ma_result wsola_on_seek(ma_data_source* pDS, ma_uint64 frameIndex);
static ma_result wsola_on_get_data_format(ma_data_source* pDS, ma_format* pFormat,
                                           ma_uint32* pChannels, ma_uint32* pSampleRate,
                                           ma_channel* pChannelMap, size_t channelMapCap);
static ma_result wsola_on_get_cursor(ma_data_source* pDS, ma_uint64* pCursor);
static ma_result wsola_on_get_length(ma_data_source* pDS, ma_uint64* pLength);

static ma_data_source_vtable s_vtable = {
    wsola_on_read,
    wsola_on_seek,
    wsola_on_get_data_format,
    wsola_on_get_cursor,
    wsola_on_get_length,
    NULL,   // onSetLooping
    0,      // flags
};

// ---- core WSOLA step --------------------------------------------------------

// Generate one synthesis hop (WSOLA_HOP frames) into ws->output_buf.
// Called from the audio thread.
static void wsola_step(WsolaSource* ws)
{
    const int    FRAME  = WSOLA_FRAME;
    const int    HOP    = WSOLA_HOP;
    const int    SEARCH = WSOLA_SEARCH;
    const uint32_t ch   = ws->channels;

    double input_pos = ws->input_pos;
    float  speed     = ws->speed.load(std::memory_order_relaxed);

    // 1. Find the best analysis position by maximizing cross-correlation
    //    between the candidate input frame and the current overlap tail.
    int best_delta = 0;
    if (!ws->first_frame && ws->frame_count >= (uint64_t)HOP) {
        float best_corr = -FLT_MAX;
        for (int d = -SEARCH; d <= SEARCH; d++) {
            int64_t cand = (int64_t)input_pos + d;
            if (cand < 0 || (uint64_t)(cand + HOP) > ws->frame_count) continue;
            float corr = 0.0f;
            const float* src = ws->pcm + cand * ch;
            const float* ref = ws->synth_buf;   // current overlap tail
            for (int i = 0; i < HOP; i++) {
                for (uint32_t c = 0; c < ch; c++)
                    corr += ref[i * ch + c] * src[i * ch + c];
            }
            if (corr > best_corr) { best_corr = corr; best_delta = d; }
        }
    }

    // 2. Clamp the final read position so it never goes out of bounds.
    int64_t read_pos = (int64_t)input_pos + best_delta;
    if (read_pos < 0) read_pos = 0;
    if ((int64_t)ws->frame_count >= FRAME) {
        int64_t max_pos = (int64_t)ws->frame_count - FRAME;
        if (read_pos > max_pos) read_pos = max_pos;
    } else {
        read_pos = 0;
    }

    // 3. Overlap-add: Hann-windowed input frame into synth_buf.
    for (int i = 0; i < FRAME; i++) {
        int64_t fi = read_pos + i;
        if (fi >= 0 && (uint64_t)fi < ws->frame_count) {
            float w = hann(i, FRAME);
            const float* src = ws->pcm + fi * ch;
            for (uint32_t c = 0; c < ch; c++)
                ws->synth_buf[i * ch + c] += src[c] * w;
        }
    }

    // 4. Copy the first HOP frames to the output staging buffer.
    memcpy(ws->output_buf, ws->synth_buf, HOP * ch * sizeof(float));

    // 5. Shift synth_buf left by HOP (keep the overlap tail for the next step).
    int tail = FRAME - HOP;
    memmove(ws->synth_buf, ws->synth_buf + HOP * ch,
            tail * ch * sizeof(float));
    memset(ws->synth_buf + tail * ch, 0, HOP * ch * sizeof(float));

    // 6. Advance the input read head by the analysis hop (HA = HOP * speed).
    ws->input_pos += (double)HOP * speed;
    ws->cursor_frames.store((uint64_t)ws->input_pos, std::memory_order_relaxed);
    ws->output_pending = HOP;
    ws->output_offset  = 0;
    ws->first_frame    = false;
}

// ---- vtable implementations -------------------------------------------------

static ma_result wsola_on_read(ma_data_source* pDS, void* pFramesOut,
                                ma_uint64 frameCount, ma_uint64* pFramesRead)
{
    WsolaSource* ws  = (WsolaSource*)pDS;
    float*       out = (float*)pFramesOut;
    uint32_t     ch  = ws->channels;
    ma_uint64    written = 0;

    while (written < frameCount) {
        // Refill staging buffer when exhausted.
        if (ws->output_offset >= ws->output_pending) {
            if (ws->input_pos >= (double)ws->frame_count) break;  // EOF
            wsola_step(ws);
        }
        int       avail = ws->output_pending - ws->output_offset;
        ma_uint64 need  = frameCount - written;
        int       take  = ((ma_uint64)avail < need) ? avail : (int)need;

        memcpy(out + written * ch,
               ws->output_buf + ws->output_offset * ch,
               take * ch * sizeof(float));
        ws->output_offset += take;
        written           += take;
    }

    if (pFramesRead) *pFramesRead = written;
    return (written < frameCount) ? MA_AT_END : MA_SUCCESS;
}

static ma_result wsola_on_seek(ma_data_source* pDS, ma_uint64 frameIndex)
{
    WsolaSource* ws = (WsolaSource*)pDS;
    ws->input_pos      = (double)frameIndex;
    ws->output_pending = 0;
    ws->output_offset  = 0;
    ws->first_frame    = true;
    ws->cursor_frames.store(frameIndex, std::memory_order_relaxed);
    memset(ws->synth_buf, 0, sizeof(ws->synth_buf));
    return MA_SUCCESS;
}

static ma_result wsola_on_get_data_format(ma_data_source* pDS, ma_format* pFormat,
                                           ma_uint32* pChannels, ma_uint32* pSampleRate,
                                           ma_channel* pChannelMap, size_t channelMapCap)
{
    WsolaSource* ws = (WsolaSource*)pDS;
    if (pFormat)     *pFormat     = ma_format_f32;
    if (pChannels)   *pChannels   = ws->channels;
    if (pSampleRate) *pSampleRate = ws->sample_rate;
    if (pChannelMap)
        ma_channel_map_init_standard(ma_standard_channel_map_default,
                                     pChannelMap, channelMapCap, ws->channels);
    return MA_SUCCESS;
}

static ma_result wsola_on_get_cursor(ma_data_source* pDS, ma_uint64* pCursor)
{
    WsolaSource* ws = (WsolaSource*)pDS;
    *pCursor = ws->cursor_frames.load(std::memory_order_relaxed);
    return MA_SUCCESS;
}

static ma_result wsola_on_get_length(ma_data_source* pDS, ma_uint64* pLength)
{
    WsolaSource* ws = (WsolaSource*)pDS;
    *pLength = ws->frame_count;
    return MA_SUCCESS;
}

// ---- public API -------------------------------------------------------------

bool wsola_init(WsolaSource* ws, float* pcm, uint64_t frames,
                uint32_t channels, uint32_t sample_rate, bool owns_pcm)
{
    // Zero only the POD members — memset on a struct with std::atomic members is UB.
    memset(&ws->base,           0, sizeof(ws->base));
    memset(ws->synth_buf,       0, sizeof(ws->synth_buf));
    memset(ws->output_buf,      0, sizeof(ws->output_buf));
    ws->pcm            = nullptr;
    ws->frame_count    = 0;
    ws->channels       = 0;
    ws->sample_rate    = 0;
    ws->owns_pcm       = false;
    ws->input_pos      = 0.0;
    ws->output_pending = 0;
    ws->output_offset  = 0;
    ws->first_frame    = false;
    ws->speed.store(1.0f, std::memory_order_relaxed);
    ws->cursor_frames.store(0, std::memory_order_relaxed);

    ma_data_source_config cfg = ma_data_source_config_init();
    cfg.vtable = &s_vtable;
    if (ma_data_source_init(&cfg, &ws->base) != MA_SUCCESS) return false;

    ws->pcm         = pcm;
    ws->frame_count = frames;
    ws->channels    = (channels > WSOLA_MAX_CH) ? WSOLA_MAX_CH : channels;
    ws->sample_rate = sample_rate;
    ws->owns_pcm    = owns_pcm;
    ws->first_frame = true;
    return true;
}

void wsola_uninit(WsolaSource* ws)
{
    ma_data_source_uninit(&ws->base);
    if (ws->owns_pcm && ws->pcm) {
        free(ws->pcm);
        ws->pcm = nullptr;
    }
}

void wsola_set_speed(WsolaSource* ws, float speed)
{
    ws->speed.store(speed, std::memory_order_relaxed);
}

float wsola_get_speed(const WsolaSource* ws)
{
    return ws->speed.load(std::memory_order_relaxed);
}
