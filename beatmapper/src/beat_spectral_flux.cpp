// beat_spectral_flux.cpp
// Beat detection via:
//   1. Spectral flux onset detection function (ODF)
//   2. Autocorrelation tempo estimation (or seed beats if provided)
//   3. Ellis DP beat tracker — regularises beats, fills quiet bars
//   4. Phase alignment to existing accepted beats (when seeds are given)
//   5. Pre-onset shift: each beat is placed a configurable amount before the
//      detected peak, landing in the quiet moment just before the attack.
//
// "Find the dominant beats, not every snap crackle and pop."

#include "beat_algo.h"
#include "chroma_fft.h"   // shared inline Cooley-Tukey FFT
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const int BF_FFT  = 2048;
static const int BF_HOP  = 512;
static const int BF_BINS = BF_FFT / 2;

// ---------------------------------------------------------------------------
// Step 1: compute spectral flux ODF for the audio in [t_start, t_end].
// Returns heap-allocated float array; caller must free().
// ---------------------------------------------------------------------------
static float* compute_flux(const float* pcm, uint64_t total_frames,
                           uint32_t channels, uint32_t sample_rate,
                           double t_start, double t_end,
                           int* out_n)
{
    *out_n = 0;
    if (!pcm || total_frames == 0 || sample_rate == 0 || channels == 0) return nullptr;

    uint64_t s0 = (uint64_t)(t_start * sample_rate);
    uint64_t s1 = (uint64_t)(t_end   * sample_rate);
    if (s1 > total_frames) s1 = total_frames;
    if (s0 >= s1 || s1 - s0 < (uint64_t)BF_FFT) return nullptr;

    int n = (int)((s1 - s0 - BF_FFT) / BF_HOP) + 1;
    if (n <= 0) return nullptr;

    float* flux = (float*)calloc(n, sizeof(float));
    float* re   = (float*)malloc(BF_FFT * sizeof(float));
    float* im   = (float*)malloc(BF_FFT * sizeof(float));
    float* prev = (float*)calloc(BF_BINS, sizeof(float));
    if (!flux || !re || !im || !prev) {
        free(flux); free(re); free(im); free(prev); return nullptr;
    }

    // Hann window
    float window[BF_FFT];
    for (int i = 0; i < BF_FFT; i++)
        window[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (BF_FFT - 1)));

    for (int f = 0; f < n; f++) {
        uint64_t off = s0 + (uint64_t)f * BF_HOP;
        // Mix to mono (handles any channel count)
        for (int i = 0; i < BF_FFT; i++) {
            uint64_t fi = off + i;
            float s = 0.0f;
            if (fi < total_frames) {
                for (uint32_t ch = 0; ch < channels; ch++)
                    s += pcm[fi * channels + ch];
                s /= (float)channels;
            }
            re[i] = s * window[i];
            im[i] = 0.0f;
        }
        chroma_fft(re, im, BF_FFT);

        // Spectral flux: sum of positive magnitude differences (half-wave rectified)
        float sf = 0.0f;
        for (int b = 0; b < BF_BINS; b++) {
            float mag  = sqrtf(re[b]*re[b] + im[b]*im[b]);
            float diff = mag - prev[b];
            if (diff > 0.0f) sf += diff;
            prev[b] = mag;
        }
        flux[f] = sf;
    }

    free(re); free(im); free(prev);
    *out_n = n;
    return flux;
}

// ---------------------------------------------------------------------------
// Step 2a: collect raw onset times (local maxima above adaptive threshold).
// ---------------------------------------------------------------------------
static void find_onsets(const float* flux, int n, double t_start,
                        uint32_t sample_rate, float threshold_mult,
                        double* times, int* count, int max_count)
{
    *count = 0;
    if (n < 3) return;

    // Adaptive threshold: mean + threshold_mult * std
    float sum = 0.0f, sum2 = 0.0f;
    for (int i = 0; i < n; i++) { sum += flux[i]; sum2 += flux[i]*flux[i]; }
    float mean = sum / n;
    float var  = sum2 / n - mean * mean;
    float std  = (var > 0.0f) ? sqrtf(var) : 1e-9f;
    float thr  = mean + threshold_mult * std;

    // Minimum gap between onsets (~50 ms)
    double hop_sec  = (double)BF_HOP / sample_rate;
    int    min_gap  = (int)(0.05 / hop_sec);
    if (min_gap < 1) min_gap = 1;
    int    last     = -min_gap * 2;

    for (int i = 1; i < n - 1 && *count < max_count; i++) {
        if (flux[i] > thr && flux[i] > flux[i-1] && flux[i] >= flux[i+1]
                && (i - last) >= min_gap) {
            times[(*count)++] = t_start + (double)i * hop_sec;
            last = i;
        }
    }
}

// ---------------------------------------------------------------------------
// Step 2b: estimate beat period in ODF frames via autocorrelation.
// Checks whether double the best lag is also strong (avoids subdivision lock).
// ---------------------------------------------------------------------------
static float estimate_period(const float* flux, int n,
                              float min_bpm, float max_bpm,
                              uint32_t sample_rate)
{
    float fps    = (float)sample_rate / BF_HOP;   // ODF frames per second
    int lag_min  = (int)(fps * 60.0f / max_bpm);
    int lag_max  = (int)(fps * 60.0f / min_bpm);
    if (lag_min < 1)    lag_min = 1;
    if (lag_max >= n)   lag_max = n - 1;
    if (lag_min > lag_max) return fps;  // fallback to 1 BPS

    float best_val = -1.0f;
    int   best_lag =  lag_min;

    for (int lag = lag_min; lag <= lag_max; lag++) {
        int   cnt = n - lag;
        float ac  = 0.0f;
        for (int i = 0; i < cnt; i++) ac += flux[i] * flux[i + lag];
        ac /= (float)cnt;
        if (ac > best_val) { best_val = ac; best_lag = lag; }
    }

    // Prefer double the period if it's nearly as strong (avoids 8th-note lock).
    int double_lag = best_lag * 2;
    if (double_lag <= lag_max) {
        int   cnt = n - double_lag;
        float ac2 = 0.0f;
        if (cnt > 0) {
            for (int i = 0; i < cnt; i++) ac2 += flux[i] * flux[i + double_lag];
            ac2 /= (float)cnt;
            if (ac2 >= best_val * 0.70f)
                best_lag = double_lag;
        }
    }

    return (float)best_lag;
}

// ---------------------------------------------------------------------------
// Step 3: median of an array (in-place sort, returns mid element).
// For small arrays only.
// ---------------------------------------------------------------------------
static float array_median(float* a, int n) {
    for (int i = 0; i < n - 1; i++) {
        int m = i;
        for (int j = i + 1; j < n; j++) if (a[j] < a[m]) m = j;
        float t = a[i]; a[i] = a[m]; a[m] = t;
    }
    return a[n / 2];
}

// ---------------------------------------------------------------------------
// Step 4: Ellis-style DP beat tracker.
// C[t] = ODF[t] + max_{t'} [ C[t'] - tightness * log(delta/tau)^2 ]
// where delta = t - t'.  Quiet beats are "filled in" because the global
// temporal-consistency bonus propagates across low-ODF frames.
// ---------------------------------------------------------------------------
static void dp_beat_track(const float* flux, int n, float tau, float tightness,
                          int* beat_frames, int* beat_count, int max_beats)
{
    *beat_count = 0;
    if (n < 2 || tau < 1.0f) return;

    float* score = (float*)malloc(n * sizeof(float));
    int*   prev  = (int*)  malloc(n * sizeof(int));
    if (!score || !prev) { free(score); free(prev); return; }

    for (int t = 0; t < n; t++) { score[t] = flux[t]; prev[t] = -1; }

    for (int t = 1; t < n; t++) {
        // Search range: 0.5 * tau  to  2.5 * tau back
        int t_lo = (int)(t - 2.5f * tau);
        int t_hi = (int)(t - 0.5f * tau);
        if (t_lo < 0) t_lo = 0;
        if (t_hi < 0 || t_hi >= t) { t_hi = t - 1; }

        float best_val = -1e30f;
        int   best_p   = -1;
        for (int p = t_lo; p <= t_hi; p++) {
            float delta = (float)(t - p);
            float ratio = delta / tau;
            float logr  = logf(ratio);
            float val   = score[p] - tightness * logr * logr;
            if (val > best_val) { best_val = val; best_p = p; }
        }
        if (best_p >= 0) {
            score[t] = flux[t] + best_val;
            prev[t]  = best_p;
        }
    }

    // Find best end frame (search last half of track)
    float best_sc = -1e30f;
    int   end_t   = n - 1;
    for (int t = n / 2; t < n; t++) {
        if (score[t] > best_sc) { best_sc = score[t]; end_t = t; }
    }

    // Backtrack
    int tmp[MAX_BEAT_CANDS];
    int cnt = 0;
    int t   = end_t;
    while (t >= 0 && cnt < max_beats) {
        tmp[cnt++] = t;
        t = prev[t];
    }
    // Reverse to chronological order
    for (int i = 0; i < cnt; i++) beat_frames[i] = tmp[cnt - 1 - i];
    *beat_count = cnt;

    free(score); free(prev);
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
void beat_spectral_flux(const float* pcm, uint64_t frame_count,
                        uint32_t channels, uint32_t sample_rate,
                        double t_start, double t_end,
                        const BeatAlgoParams* params,
                        AutoBeatList* out)
{
    out->beat_count    = 0;
    out->onset_count   = 0;
    out->estimated_bpm = 0.0f;
    if (!pcm || frame_count == 0 || sample_rate == 0) return;

    float min_bpm  = (params->min_bpm  > 0.0f) ? params->min_bpm  : 60.0f;
    float max_bpm  = (params->max_bpm  > 0.0f) ? params->max_bpm  : 200.0f;
    float thresh   = (params->onset_threshold > 0.0f) ? params->onset_threshold : 1.5f;
    float tight    = (params->dp_tightness   > 0.0f) ? params->dp_tightness   : 400.0f;
    double pre_sec = (double)params->pre_onset_ms / 1000.0;

    // 1. Spectral flux ODF
    int    n_flux = 0;
    float* flux   = compute_flux(pcm, frame_count, channels, sample_rate,
                                 t_start, t_end, &n_flux);
    if (!flux || n_flux < 4) { free(flux); return; }

    double hop_sec = (double)BF_HOP / sample_rate;
    float  fps     = (float)sample_rate / BF_HOP;

    // 2. Raw onsets (always computed; used for display with show_raw_onsets)
    find_onsets(flux, n_flux, t_start, sample_rate, thresh,
                out->onset_times, &out->onset_count, MAX_BEAT_CANDS);

    // 3. Period estimation
    float tau;  // expected beat period in ODF frames
    if (params->seed_count >= 2 && params->seed_times) {
        float ibis[MAX_BEAT_CANDS];
        int   n_ibis = 0;
        for (int i = 1; i < params->seed_count && n_ibis < MAX_BEAT_CANDS; i++) {
            double ibi = params->seed_times[i] - params->seed_times[i - 1];
            if (ibi > 0.08 && ibi < 5.0)
                ibis[n_ibis++] = (float)ibi;
        }
        tau = (n_ibis > 0) ? array_median(ibis, n_ibis) * fps
                           : estimate_period(flux, n_flux, min_bpm, max_bpm, sample_rate);
    } else {
        tau = estimate_period(flux, n_flux, min_bpm, max_bpm, sample_rate);
    }
    if (tau < 1.0f) tau = 1.0f;

    double tau_sec = tau * hop_sec;
    out->estimated_bpm = 60.0f / (float)tau_sec;

    // 3.5. Grid bias: when seed beats are present, reshape the ODF so the DP
    //      strongly prefers frames that lie on the established beat grid and
    //      is significantly penalised for the halfway (subdivision) positions.
    //
    //      a) Best-fit anchor: find the phase origin that minimises the RMS
    //         distance from each seed beat to its nearest grid position.
    //      b) Steadiness: if the seeds are very regular (low RMS phase error),
    //         the bias amplitude is high.  Irregular seeds produce a weaker bias
    //         so the ODF still has some say.
    //      c) Cosine bias:  bias[f] = amplitude * cos(2π * phase_f)
    //           phase_f = 0   → on-grid  → +amplitude  (bonus)
    //           phase_f = 0.5 → halfway  → -amplitude  (penalty)
    //         At full steadiness a halfway transient must exceed ~4× the mean
    //         ODF to compete with a quiet on-grid frame.

    double seed_anchor = 0.0;  // reused in step 5
    float  steadiness  = 0.0f;

    if (params->seed_count >= 2 && params->seed_times) {
        const double* seeds = params->seed_times;
        int           ns    = params->seed_count;

        // a) Iteratively refine anchor toward mean-residual minimum
        seed_anchor = seeds[0];
        for (int iter = 0; iter < 4; iter++) {
            double err_sum = 0.0;
            for (int j = 0; j < ns; j++) {
                double off = seeds[j] - seed_anchor;
                err_sum += off - round(off / tau_sec) * tau_sec;
            }
            seed_anchor += err_sum / ns;
        }

        // b) RMS phase error → steadiness in [0, 1]
        //    Quarter-beat jitter (0.25 * tau) maps to steadiness = 0.
        double rms2 = 0.0;
        for (int j = 0; j < ns; j++) {
            double off = seeds[j] - seed_anchor;
            double err = off - round(off / tau_sec) * tau_sec;
            rms2 += err * err;
        }
        float rms_ratio = (float)(sqrt(rms2 / ns) / (0.25 * tau_sec));
        steadiness = 1.0f - fminf(1.0f, rms_ratio);
        steadiness *= steadiness;  // square: very steady grids get a big boost

        // c) Apply cosine bias in-place
        if (steadiness > 0.01f) {
            float flux_mean = 0.0f;
            for (int f = 0; f < n_flux; f++) flux_mean += flux[f];
            if (n_flux > 0) flux_mean /= n_flux;

            float amplitude = steadiness * 4.0f * flux_mean;
            for (int f = 0; f < n_flux; f++) {
                double phase_sec = fmod(t_start + (double)f * hop_sec - seed_anchor,
                                        tau_sec);
                if (phase_sec < 0.0) phase_sec += tau_sec;
                float bias = amplitude * cosf(2.0f * 3.14159265f *
                                              (float)(phase_sec / tau_sec));
                flux[f] = fmaxf(0.0f, flux[f] + bias);
            }
        }
    }

    // 4. DP beat tracking on the (possibly biased) ODF
    int beat_frames[MAX_BEAT_CANDS];
    int beat_count  = 0;
    dp_beat_track(flux, n_flux, tau, tight, beat_frames, &beat_count, MAX_BEAT_CANDS);
    free(flux);
    flux = nullptr;

    if (beat_count == 0) return;

    // 5. Fine-tune: shift all DP beats by the median residual offset to seed
    //    beats.  The grid bias has already done the heavy lifting; this corrects
    //    any remaining sub-frame systematic error.
    if (params->seed_count >= 2 && params->seed_times) {
        const double* seeds = params->seed_times;
        int           ns    = params->seed_count;
        float offsets[MAX_BEAT_CANDS];
        int   n_off = 0;

        for (int j = 0; j < ns && n_off < MAX_BEAT_CANDS; j++) {
            double best_dist = 1e30, best_off = 0.0;
            for (int i = 0; i < beat_count; i++) {
                double dp_t = t_start + (double)beat_frames[i] * hop_sec;
                double off  = seeds[j] - dp_t;
                off -= tau_sec * round(off / tau_sec);
                if (fabs(off) < best_dist) { best_dist = fabs(off); best_off = off; }
            }
            offsets[n_off++] = (float)best_off;
        }
        if (n_off > 0) {
            int frame_shift = (int)(array_median(offsets, n_off) / hop_sec + 0.5);
            for (int i = 0; i < beat_count; i++) {
                beat_frames[i] += frame_shift;
                if (beat_frames[i] < 0)      beat_frames[i] = 0;
                if (beat_frames[i] >= n_flux) beat_frames[i] = n_flux - 1;
            }
        }
    }

    // 6. Convert frame indices to times; apply pre-onset shift; store output
    int cnt = 0;
    for (int i = 0; i < beat_count && cnt < MAX_BEAT_CANDS; i++) {
        double t = t_start + (double)beat_frames[i] * hop_sec - pre_sec;
        if (t < 0.0) t = 0.0;
        out->beat_times[cnt]    = t;
        out->beat_selected[cnt] = true;
        cnt++;
    }
    out->beat_count = cnt;
}
