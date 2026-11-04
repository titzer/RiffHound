#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"

#include "audio_io.h"

#include <atomic>
#include <math.h>
#include <stdio.h>
#include <string.h>

const char* const TONE_WAVE_NAMES[WAVE_COUNT] = { "Sine", "Triangle", "Square", "Sawtooth" };

static ma_context s_ctx;
static bool       s_ctx_ok = false;
static char       s_error[256] = "";

// ---------------------------------------------------------------------------
// Capture

static const int RING = 1 << 16;            // > 1 s at 48 kHz; must be a power of two
static float                  s_ring[RING];
static std::atomic<uint64_t>  s_written{0};  // total samples written since start
static ma_device              s_cap;
static bool                   s_cap_open = false;
static float                  s_cap_rate = 48000.0f;

static void capture_cb(ma_device*, void*, const void* input, ma_uint32 frames) {
    const float* in = (const float*)input;
    uint64_t w = s_written.load(std::memory_order_relaxed);
    for (ma_uint32 i = 0; i < frames; i++) s_ring[(w + i) & (RING - 1)] = in[i];
    s_written.store(w + frames, std::memory_order_release);
}

bool audio_init() {
    if (ma_context_init(nullptr, 0, nullptr, &s_ctx) != MA_SUCCESS) {
        snprintf(s_error, sizeof(s_error), "audio system unavailable");
        return false;
    }
    s_ctx_ok = true;
    return true;
}

std::vector<std::string> capture_device_names() {
    std::vector<std::string> names;
    if (!s_ctx_ok) return names;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&s_ctx, nullptr, nullptr, &infos, &count) != MA_SUCCESS) return names;
    for (ma_uint32 i = 0; i < count; i++) names.push_back(infos[i].name);
    return names;
}

bool capture_start(const std::string& device_name) {
    capture_stop();
    if (!s_ctx_ok) return false;

    ma_device_id id;
    bool have_id = false;
    if (!device_name.empty()) {
        ma_device_info* infos = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(&s_ctx, nullptr, nullptr, &infos, &count) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count; i++)
                if (device_name == infos[i].name) { id = infos[i].id; have_id = true; break; }
        }
        // A device that went away falls back to the default rather than failing.
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.pDeviceID = have_id ? &id : nullptr;
    cfg.capture.format    = ma_format_f32;
    cfg.capture.channels  = 1;
    cfg.sampleRate        = 0;                 // device native rate
    cfg.periodSizeInMilliseconds = 10;
    cfg.dataCallback      = capture_cb;
    if (ma_device_init(&s_ctx, &cfg, &s_cap) != MA_SUCCESS) {
        snprintf(s_error, sizeof(s_error), "cannot open input device");
        return false;
    }
    s_cap_rate = (float)s_cap.sampleRate;
    s_written.store(0);
    if (ma_device_start(&s_cap) != MA_SUCCESS) {
        ma_device_uninit(&s_cap);
        snprintf(s_error, sizeof(s_error), "cannot start input device");
        return false;
    }
    s_cap_open = true;
    s_error[0] = '\0';
    return true;
}

void capture_stop() {
    if (!s_cap_open) return;
    ma_device_uninit(&s_cap);
    s_cap_open = false;
}

bool  capture_running()     { return s_cap_open; }
float capture_sample_rate() { return s_cap_rate; }
const char* audio_error()   { return s_error; }

bool capture_latest(float* out, int n) {
    if (!s_cap_open || n > RING / 2) return false;
    uint64_t w = s_written.load(std::memory_order_acquire);
    if (w < (uint64_t)n) return false;
    uint64_t start = w - n;
    for (int i = 0; i < n; i++) out[i] = s_ring[(start + i) & (RING - 1)];
    return true;
}

// ---------------------------------------------------------------------------
// Reference tone

static ma_device          s_play;
static bool               s_play_open = false;
static std::atomic<float> s_tone_freq{0.0f};
static std::atomic<float> s_tone_vol{0.0f};
static std::atomic<int>   s_tone_wave{WAVE_SINE};

// polyBLEP residual: removes most of the aliasing from the discontinuities
// of the naive square and sawtooth.
static inline double blep(double t, double dt) {
    if (t < dt)       { t /= dt;             return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt;  return t * t + t + t + 1.0; }
    return 0.0;
}

static void playback_cb(ma_device* dev, void* output, const void*, ma_uint32 frames) {
    static double phase = 0.0, gain = 0.0, freq = 440.0, tri = 0.0;
    float* out = (float*)output;
    const double sr = dev->sampleRate;
    const float  target_f = s_tone_freq.load(std::memory_order_relaxed);
    const double target_g = target_f > 0 ? s_tone_vol.load(std::memory_order_relaxed) : 0.0;
    const int    wave = s_tone_wave.load(std::memory_order_relaxed);
    if (target_f > 0) freq = target_f;          // hold the pitch while fading out
    const double k  = 1.0 - exp(-1.0 / (0.015 * sr));   // 15 ms gain ramp
    const double dt = freq / sr;
    for (ma_uint32 i = 0; i < frames; i++) {
        gain += (target_g - gain) * k;
        double v;
        switch (wave) {
        case WAVE_TRIANGLE: {
            // Integrated band-limited square, leaky to stay centred.
            double sq = (phase < 0.5 ? 1.0 : -1.0) + blep(phase, dt) - blep(fmod(phase + 0.5, 1.0), dt);
            tri = dt * 4.0 * sq + (1.0 - 0.001) * tri;
            v = tri;
            break;
        }
        case WAVE_SQUARE:
            v = (phase < 0.5 ? 1.0 : -1.0) + blep(phase, dt) - blep(fmod(phase + 0.5, 1.0), dt);
            v *= 0.5;   // equal-loudness-ish against the sine
            break;
        case WAVE_SAW:
            v = (2.0 * phase - 1.0) - blep(phase, dt);
            v *= 0.6;
            break;
        default:
            v = sin(2.0 * M_PI * phase);
            break;
        }
        phase += dt;
        if (phase >= 1.0) phase -= 1.0;
        out[i] = (float)(v * gain);
    }
}

void tone_set(float freq, int wave, float volume) {
    if (freq > 0 && !s_play_open && s_ctx_ok) {
        // Opened on first use and left running: a stopped output device
        // would add start-up latency to every key press.
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format   = ma_format_f32;
        cfg.playback.channels = 1;
        cfg.sampleRate        = 0;
        cfg.periodSizeInMilliseconds = 10;
        cfg.dataCallback      = playback_cb;
        if (ma_device_init(&s_ctx, &cfg, &s_play) == MA_SUCCESS) {
            if (ma_device_start(&s_play) == MA_SUCCESS) s_play_open = true;
            else ma_device_uninit(&s_play);
        }
    }
    s_tone_wave.store(wave);
    s_tone_vol.store(volume);
    s_tone_freq.store(freq);
}

void audio_shutdown() {
    capture_stop();
    if (s_play_open) { ma_device_uninit(&s_play); s_play_open = false; }
    if (s_ctx_ok) { ma_context_uninit(&s_ctx); s_ctx_ok = false; }
}
