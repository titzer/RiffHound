#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio.h"
#include "stretch.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifdef __APPLE__
#include <AudioToolbox/ExtendedAudioFile.h>
#include <CoreFoundation/CoreFoundation.h>
#include <strings.h>  // strcasecmp
#endif

static ma_engine     s_engine;
static ma_sound      s_sound;
static StretchSource s_stretch;
static bool          s_engine_ok  = false;
static bool          s_sound_ok   = false;
static bool          s_stretch_ok = false;

#ifdef __APPLE__
static bool path_is_m4a(const char* p) {
    const char* dot = strrchr(p, '.');
    return dot && strcasecmp(dot, ".m4a") == 0;
}

// Decode M4A/AAC to float32 interleaved PCM at the file's native sample rate.
// Caller must free *out_pcm with free().
static bool decode_m4a_to_f32(const char* path, float** out_pcm,
                                uint64_t* out_frames,
                                ma_uint32* out_channels, uint32_t* out_sr)
{
    CFStringRef str = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef    url = CFURLCreateWithFileSystemPath(NULL, str, kCFURLPOSIXPathStyle, false);
    CFRelease(str);

    ExtAudioFileRef ef = NULL;
    OSStatus err = ExtAudioFileOpenURL(url, &ef);
    CFRelease(url);
    if (err != noErr) return false;

    // Get source format (sample rate, channel count)
    AudioStreamBasicDescription srcFmt = {};
    UInt32 sz = sizeof(srcFmt);
    ExtAudioFileGetProperty(ef, kExtAudioFileProperty_FileDataFormat, &sz, &srcFmt);

    ma_uint32 nch = (ma_uint32)(srcFmt.mChannelsPerFrame > 0 ? srcFmt.mChannelsPerFrame : 1);
    uint32_t  sr  = (uint32_t)(srcFmt.mSampleRate > 0 ? srcFmt.mSampleRate : 44100);

    // Request float32 interleaved at native rate and channel count
    AudioStreamBasicDescription outFmt = {};
    outFmt.mSampleRate       = srcFmt.mSampleRate;
    outFmt.mFormatID         = kAudioFormatLinearPCM;
    outFmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    outFmt.mBitsPerChannel   = 32;
    outFmt.mChannelsPerFrame = nch;
    outFmt.mBytesPerFrame    = 4 * nch;
    outFmt.mFramesPerPacket  = 1;
    outFmt.mBytesPerPacket   = 4 * nch;
    ExtAudioFileSetProperty(ef, kExtAudioFileProperty_ClientDataFormat, sizeof(outFmt), &outFmt);

    // Estimate total frames for initial allocation
    SInt64 estFrames = 0;
    sz = sizeof(estFrames);
    ExtAudioFileGetProperty(ef, kExtAudioFileProperty_FileLengthFrames, &sz, &estFrames);
    if (estFrames <= 0) estFrames = (SInt64)sr * 600;

    size_t capacity = (size_t)estFrames + 4096;
    float* buf = (float*)malloc(capacity * nch * sizeof(float));
    if (!buf) { ExtAudioFileDispose(ef); return false; }

    const UInt32 CHUNK = 65536;
    uint64_t total = 0;
    for (;;) {
        if (total + CHUNK > capacity) {
            capacity *= 2;
            float* tmp = (float*)realloc(buf, capacity * nch * sizeof(float));
            if (!tmp) { free(buf); ExtAudioFileDispose(ef); return false; }
            buf = tmp;
        }
        AudioBufferList abl;
        abl.mNumberBuffers              = 1;
        abl.mBuffers[0].mNumberChannels = nch;
        abl.mBuffers[0].mDataByteSize   = CHUNK * nch * sizeof(float);
        abl.mBuffers[0].mData           = buf + total * nch;
        UInt32 n = CHUNK;
        ExtAudioFileRead(ef, &n, &abl);
        if (n == 0) break;
        total += n;
    }
    ExtAudioFileDispose(ef);

    if (total == 0) { free(buf); return false; }
    *out_pcm      = buf;
    *out_frames   = total;
    *out_channels = nch;
    *out_sr       = sr;
    return true;
}
#endif  // __APPLE__

// Decode any supported file to stereo (2-channel) f32 PCM at the native sample
// rate.  miniaudio handles mono→stereo upmix for mp3/wav; M4A channel conversion
// is done here.  Caller must free *out_pcm with free().
static bool decode_to_pcm_stereo(const char* path, float** out_pcm,
                                   uint64_t* out_frames, uint32_t* out_sr)
{
#ifdef __APPLE__
    if (path_is_m4a(path)) {
        float*    pcm;
        uint64_t  frames;
        ma_uint32 nch;
        uint32_t  sr;
        if (!decode_m4a_to_f32(path, &pcm, &frames, &nch, &sr)) return false;

        if (nch != 2) {
            // Convert to stereo
            float* stereo = (float*)malloc(frames * 2 * sizeof(float));
            if (!stereo) { free(pcm); return false; }
            for (uint64_t i = 0; i < frames; i++) {
                float l = (nch >= 1) ? pcm[i * nch + 0] : 0.0f;
                float r = (nch >= 2) ? pcm[i * nch + 1] : l;
                stereo[i * 2 + 0] = l;
                stereo[i * 2 + 1] = r;
            }
            free(pcm);
            pcm = stereo;
        }
        *out_pcm    = pcm;
        *out_frames = frames;
        *out_sr     = sr;
        return true;
    }
#endif

    // Decode mp3/wav/etc. to stereo f32 at the file's native sample rate.
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file(path, &cfg, &decoder) != MA_SUCCESS) return false;

    uint32_t sr = decoder.outputSampleRate;

    const ma_uint64 CHUNK = 65536;
    float*  buf      = NULL;
    size_t  total    = 0;
    size_t  capacity = 0;

    for (;;) {
        if (total + CHUNK > capacity) {
            size_t new_cap = (capacity == 0) ? CHUNK * 16 : capacity * 2;
            float* tmp = (float*)realloc(buf, new_cap * 2 * sizeof(float));
            if (!tmp) { free(buf); ma_decoder_uninit(&decoder); return false; }
            buf = tmp;
            capacity = new_cap;
        }
        ma_uint64 read = 0;
        ma_result res = ma_decoder_read_pcm_frames(&decoder, buf + total * 2, CHUNK, &read);
        total += (size_t)read;
        if (res == MA_AT_END || read == 0) break;
    }

    uint32_t out_sr_val = decoder.outputSampleRate;
    ma_decoder_uninit(&decoder);

    if (total == 0) { free(buf); return false; }
    *out_pcm    = buf;
    *out_frames = (uint64_t)total;
    *out_sr     = out_sr_val;
    (void)sr;
    return true;
}

void audio_init(AudioState* a) {
    memset(a, 0, sizeof(*a));
    if (ma_engine_init(NULL, &s_engine) != MA_SUCCESS) {
        fprintf(stderr, "[audio] failed to init miniaudio engine\n");
        return;
    }
    s_engine_ok = true;
}

bool audio_load(AudioState* a, EditorState* e, const char* path) {
    if (!s_engine_ok) return false;

    // Tear down the previous sound if one was loaded.
    if (s_sound_ok) { ma_sound_uninit(&s_sound);   s_sound_ok = false; }
    if (s_stretch_ok) { stretch_uninit(&s_stretch); s_stretch_ok = false; }

    a->loaded   = false;
    a->playing  = false;
    a->position = 0.0;

    // Decode to stereo f32 PCM.
    float*   pcm;
    uint64_t frames;
    uint32_t sr;
    if (!decode_to_pcm_stereo(path, &pcm, &frames, &sr)) {
        fprintf(stderr, "[audio] failed to decode '%s'\n", path);
        return false;
    }

    // Initialise the stretch source (takes ownership of pcm).
    if (!stretch_init(&s_stretch, pcm, frames, 2, sr, /*owns_pcm=*/true)) {
        free(pcm);
        fprintf(stderr, "[audio] stretch_init failed for '%s'\n", path);
        return false;
    }
    // Reset speed and pitch to defaults on every new file load.
    e->speed     = 1.0f;
    e->semitones = 0;
    e->cents     = 0;
    stretch_set_speed(&s_stretch, 1.0f);
    stretch_set_pitch(&s_stretch, 1.0f);
    s_stretch_ok = true;

    // Initialise miniaudio sound backed by the stretch source.
    ma_result result = ma_sound_init_from_data_source(
        &s_engine, &s_stretch,
        MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
        NULL, &s_sound);
    if (result != MA_SUCCESS) {
        stretch_uninit(&s_stretch); s_stretch_ok = false;
        fprintf(stderr, "[audio] ma_sound_init_from_data_source failed: %d\n", result);
        return false;
    }
    s_sound_ok = true;

    strncpy(a->filename, path, sizeof(a->filename) - 1);
    a->filename[sizeof(a->filename) - 1] = '\0';
    a->duration = (double)frames / sr;
    a->position = 0.0;
    a->playing  = false;
    a->loaded   = true;

    printf("[audio] loaded '%s'  duration=%.2fs  ch=2  sr=%u\n",
           path, a->duration, sr);
    return true;
}

void audio_play(AudioState* a) {
    if (!s_sound_ok || !a->loaded) return;
    a->play_start = a->position;
    ma_sound_start(&s_sound);
    a->playing = true;
}

void audio_pause(AudioState* a) {
    if (!s_sound_ok) return;
    ma_sound_stop(&s_sound);
    a->playing = false;
}

void audio_seek(AudioState* a, double time_sec) {
    if (!s_sound_ok || !a->loaded) return;
    if (time_sec < 0.0)         time_sec = 0.0;
    if (time_sec > a->duration) time_sec = a->duration;

    // Seek in terms of the data source's (file's) sample rate.
    ma_uint64 frame = (ma_uint64)(time_sec * s_stretch.sample_rate + 0.5);
    ma_sound_seek_to_pcm_frame(&s_sound, frame);
    a->position = time_sec;
}

double audio_get_position(AudioState* a) {
    return a->position;
}

void audio_set_speed(EditorState* e, float speed) {
    if (speed < 0.25f) speed = 0.25f;
    if (speed > 2.00f) speed = 2.00f;
    // Round to nearest 0.05
    speed = roundf(speed * 20.0f) / 20.0f;
    e->speed = speed;
    if (s_stretch_ok) stretch_set_speed(&s_stretch, speed);
}

void audio_set_pitch(EditorState* e, int semitones, int cents) {
    if (semitones < -12) semitones = -12;
    if (semitones >  12) semitones =  12;
    if (cents < -100) cents = -100;
    if (cents >  100) cents =  100;
    e->semitones = semitones;
    e->cents     = cents;
    float ratio = powf(2.0f, (float)(semitones * 100 + cents) / 1200.0f);
    if (s_stretch_ok) stretch_set_pitch(&s_stretch, ratio);
}

void audio_set_loop(AudioState* a, bool enabled, double loop_start, double loop_end) {
    a->loop = enabled;
    if (s_stretch_ok) {
        uint64_t sf = (uint64_t)(loop_start * s_stretch.sample_rate + 0.5);
        uint64_t ef = (uint64_t)(loop_end   * s_stretch.sample_rate + 0.5);
        if (ef > s_stretch.frame_count) ef = s_stretch.frame_count;
        if (sf >= ef) sf = 0;
        stretch_set_loop(&s_stretch, enabled, sf, ef);
    }
}

void audio_set_playback_override(AudioState* a, float* pcm, uint64_t frames) {
    if (!s_stretch_ok || !a->loaded) { free(pcm); return; }
    if (pcm && frames != s_stretch.frame_count) {
        uint64_t want = s_stretch.frame_count;
        float* fit = (float*)calloc((size_t)want * 2, sizeof(float));
        if (!fit) { free(pcm); return; }
        uint64_t n = frames < want ? frames : want;
        memcpy(fit, pcm, (size_t)n * 2 * sizeof(float));
        free(pcm);
        pcm = fit;
    }
    stretch_set_override(&s_stretch, pcm);
}

bool audio_decode_stereo_at(const char* path, uint32_t sample_rate,
                            float** out_pcm, uint64_t* out_frames)
{
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, sample_rate);
    ma_decoder decoder;
    if (ma_decoder_init_file(path, &cfg, &decoder) != MA_SUCCESS) return false;
    const ma_uint64 CHUNK = 65536;
    float* buf = NULL; size_t total = 0, capacity = 0;
    for (;;) {
        if (total + CHUNK > capacity) {
            size_t new_cap = (capacity == 0) ? CHUNK * 16 : capacity * 2;
            float* tmp = (float*)realloc(buf, new_cap * 2 * sizeof(float));
            if (!tmp) { free(buf); ma_decoder_uninit(&decoder); return false; }
            buf = tmp; capacity = new_cap;
        }
        ma_uint64 read = 0;
        ma_result res = ma_decoder_read_pcm_frames(&decoder, buf + total * 2, CHUNK, &read);
        total += (size_t)read;
        if (res == MA_AT_END || read == 0) break;
    }
    ma_decoder_uninit(&decoder);
    if (total == 0) { free(buf); return false; }
    *out_pcm = buf; *out_frames = (uint64_t)total;
    return true;
}

void audio_update(AudioState* a) {
    if (s_stretch_ok) stretch_collect_retired(&s_stretch);
    if (!s_sound_ok || !a->loaded) return;

    // Sync the playing flag from the audio thread, but only allow it to go
    // false here.  audio_play() is the sole place that sets it true, so that
    // the brief async delay before ma_sound_is_playing reflects a ma_sound_stop
    // call doesn't flip playing back to true and confuse the stop/start logic.
    if (!ma_sound_is_playing(&s_sound))
        a->playing = false;

    // Read cursor from the atomic updated by the audio thread after each hop.
    // Only while playing: ma_sound_seek_to_pcm_frame is deferred (applied on
    // the audio thread's next read), so while stopped the stretch cursor still
    // holds the pre-seek position and syncing from it would snap the playhead
    // back after every click-seek -- the "Space needs several presses" bug.
    if (s_stretch_ok && a->playing) {
        uint64_t cur = s_stretch.cursor_frames.load(std::memory_order_relaxed);
        a->position = (double)cur / s_stretch.sample_rate;
    }
}

bool audio_decode_pcm(const char* path, float** out_samples,
                      uint64_t* out_frame_count, uint32_t* out_sample_rate)
{
#ifdef __APPLE__
    if (path_is_m4a(path)) {
        float*    pcm;
        uint64_t  frames;
        ma_uint32 nch;
        uint32_t  sr;
        if (!decode_m4a_to_f32(path, &pcm, &frames, &nch, &sr)) return false;
        // Mix down to mono if multichannel
        if (nch > 1) {
            for (uint64_t i = 0; i < frames; i++) {
                float s = 0.0f;
                for (ma_uint32 c = 0; c < nch; c++) s += pcm[i * nch + c];
                pcm[i] = s / (float)nch;
            }
        }
        *out_samples     = pcm;
        *out_frame_count = frames;
        *out_sample_rate = sr;
        return true;
    }
#endif

    // Decode entire file as mono f32 at the file's native sample rate.
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file(path, &cfg, &decoder) != MA_SUCCESS)
        return false;

    const ma_uint64 CHUNK = 65536;
    float*  buf      = NULL;
    size_t  total    = 0;
    size_t  capacity = 0;

    for (;;) {
        if (total + CHUNK > capacity) {
            size_t new_cap = (capacity == 0) ? CHUNK * 16 : capacity * 2;
            float* tmp = (float*)realloc(buf, new_cap * sizeof(float));
            if (!tmp) { free(buf); ma_decoder_uninit(&decoder); return false; }
            buf = tmp;
            capacity = new_cap;
        }
        ma_uint64 read = 0;
        ma_result res = ma_decoder_read_pcm_frames(&decoder, buf + total, CHUNK, &read);
        total += (size_t)read;
        if (res == MA_AT_END || read == 0) break;
    }

    uint32_t sr = decoder.outputSampleRate;
    ma_decoder_uninit(&decoder);

    if (total == 0) { free(buf); return false; }

    *out_samples     = buf;
    *out_frame_count = (uint64_t)total;
    *out_sample_rate = sr;
    return true;
}

void audio_free_pcm(float* samples) {
    free(samples);
}

const float* audio_pcm_data(const AudioState* a,
                              uint64_t* frame_count,
                              uint32_t* channels,
                              uint32_t* sample_rate) {
    if (!s_stretch_ok || !s_stretch.pcm) return nullptr;
    if (frame_count)  *frame_count  = s_stretch.frame_count;
    if (channels)     *channels     = s_stretch.channels;
    if (sample_rate)  *sample_rate  = s_stretch.sample_rate;
    return s_stretch.pcm;
}

void audio_shutdown(AudioState* a) {
    // Stop the sound (removes it from the node graph) first.
    if (s_sound_ok)  { ma_sound_uninit(&s_sound);   s_sound_ok = false; }
    // Stop the engine's audio thread BEFORE freeing the stretch source;
    // stretch_uninit frees its pcm, which the audio thread must not be reading.
    if (s_engine_ok)  { ma_engine_uninit(&s_engine);  s_engine_ok = false; }
    if (s_stretch_ok) { stretch_uninit(&s_stretch);   s_stretch_ok = false; }
    a->loaded  = false;
    a->playing = false;
}
