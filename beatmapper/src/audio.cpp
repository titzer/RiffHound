#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio.h"
#include <string.h>
#include <stdio.h>

#ifdef __APPLE__
#include <AudioToolbox/ExtendedAudioFile.h>
#include <CoreFoundation/CoreFoundation.h>
#include <strings.h>  // strcasecmp
#endif

static ma_engine s_engine;
static ma_sound  s_sound;
static bool      s_engine_ok = false;
static bool      s_sound_ok  = false;

#ifdef __APPLE__
static ma_audio_buffer s_m4a_buf;
static float*          s_m4a_pcm    = nullptr;
static bool            s_m4a_active = false;
static uint32_t        s_m4a_sr     = 0;  // native sample rate of the M4A buffer

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

void audio_init(AudioState* a) {
    memset(a, 0, sizeof(*a));
    if (ma_engine_init(NULL, &s_engine) != MA_SUCCESS) {
        fprintf(stderr, "[audio] failed to init miniaudio engine\n");
        return;
    }
    s_engine_ok = true;
}

bool audio_load(AudioState* a, const char* path) {
    if (!s_engine_ok) return false;

    // Tear down the previous sound if one was loaded.
    if (s_sound_ok) {
        ma_sound_uninit(&s_sound);
        s_sound_ok = false;
    }
#ifdef __APPLE__
    if (s_m4a_active) {
        ma_audio_buffer_uninit(&s_m4a_buf);
        free(s_m4a_pcm);
        s_m4a_pcm    = nullptr;
        s_m4a_active = false;
    }
#endif
    a->loaded   = false;
    a->playing  = false;
    a->position = 0.0;

#ifdef __APPLE__
    if (path_is_m4a(path)) {
        float*    pcm;
        uint64_t  frames;
        ma_uint32 nch;
        uint32_t  sr;
        if (!decode_m4a_to_f32(path, &pcm, &frames, &nch, &sr)) {
            fprintf(stderr, "[audio] failed to decode M4A '%s'\n", path);
            return false;
        }
        ma_audio_buffer_config cfg =
            ma_audio_buffer_config_init(ma_format_f32, nch, frames, pcm, NULL);
        if (ma_audio_buffer_init(&cfg, &s_m4a_buf) != MA_SUCCESS) {
            free(pcm);
            fprintf(stderr, "[audio] failed to create audio buffer for '%s'\n", path);
            return false;
        }
        ma_result result = ma_sound_init_from_data_source(
            &s_engine, &s_m4a_buf, MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &s_sound);
        if (result != MA_SUCCESS) {
            ma_audio_buffer_uninit(&s_m4a_buf);
            free(pcm);
            fprintf(stderr, "[audio] failed to init sound from M4A '%s': %d\n", path, result);
            return false;
        }
        s_m4a_pcm    = pcm;
        s_m4a_active = true;
        s_m4a_sr     = sr;
        s_sound_ok   = true;

        strncpy(a->filename, path, sizeof(a->filename) - 1);
        a->filename[sizeof(a->filename) - 1] = '\0';
        a->duration = (double)frames / sr;
        a->position = 0.0;
        a->playing  = false;
        a->loaded   = true;
        printf("[audio] loaded M4A '%s'  duration=%.2fs  ch=%u  sr=%u\n",
               path, a->duration, nch, sr);
        return true;
    }
#endif

    ma_result result = ma_sound_init_from_file(
        &s_engine, path,
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
        NULL, NULL, &s_sound);

    if (result != MA_SUCCESS) {
        fprintf(stderr, "[audio] failed to load '%s' (miniaudio error %d)\n", path, result);
        return false;
    }
    s_sound_ok = true;

    float length_sec = 0.0f;
    ma_sound_get_length_in_seconds(&s_sound, &length_sec);

    strncpy(a->filename, path, sizeof(a->filename) - 1);
    a->filename[sizeof(a->filename) - 1] = '\0';
    a->duration = (double)length_sec;
    a->position = 0.0;
    a->playing  = false;
    a->loaded   = true;

    printf("[audio] loaded '%s'  duration=%.2fs\n", path, a->duration);
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
    if (time_sec < 0.0)          time_sec = 0.0;
    if (time_sec > a->duration)  time_sec = a->duration;

    // For decoded (file-based) sounds the engine pre-converts to its own sample
    // rate, so seek frames must use the engine rate.  For M4A the sound is backed
    // by an ma_audio_buffer at the file's native rate, so use that rate instead.
#ifdef __APPLE__
    ma_uint32 sample_rate = s_m4a_active ? s_m4a_sr : ma_engine_get_sample_rate(&s_engine);
#else
    ma_uint32 sample_rate = ma_engine_get_sample_rate(&s_engine);
#endif
    ma_uint64 frame = (ma_uint64)(time_sec * sample_rate + 0.5);
    ma_sound_seek_to_pcm_frame(&s_sound, frame);
    a->position = time_sec;
}

double audio_get_position(AudioState* a) {
    return a->position;
}

void audio_update(AudioState* a) {
    if (!s_sound_ok || !a->loaded) return;

    // Sync playing flag from the audio thread.
    a->playing = (bool)ma_sound_is_playing(&s_sound);

    // Sync cursor position.
#ifdef __APPLE__
    // ma_sound_get_cursor_in_seconds is unreliable for data-source-backed sounds
    // (ma_sound_init_from_data_source). Query the buffer directly instead.
    if (s_m4a_active && s_m4a_sr > 0) {
        ma_uint64 cursor_frames = 0;
        if (ma_data_source_get_cursor_in_pcm_frames((ma_data_source*)&s_m4a_buf,
                                                     &cursor_frames) == MA_SUCCESS)
            a->position = (double)cursor_frames / s_m4a_sr;
        return;
    }
#endif
    float cursor_sec = 0.0f;
    if (ma_sound_get_cursor_in_seconds(&s_sound, &cursor_sec) == MA_SUCCESS)
        a->position = (double)cursor_sec;
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

void audio_shutdown(AudioState* a) {
    if (s_sound_ok) {
        ma_sound_uninit(&s_sound);
        s_sound_ok = false;
    }
#ifdef __APPLE__
    if (s_m4a_active) {
        ma_audio_buffer_uninit(&s_m4a_buf);
        free(s_m4a_pcm);
        s_m4a_pcm    = nullptr;
        s_m4a_active = false;
    }
#endif
    if (s_engine_ok) {
        ma_engine_uninit(&s_engine);
        s_engine_ok = false;
    }
    a->loaded  = false;
    a->playing = false;
}
