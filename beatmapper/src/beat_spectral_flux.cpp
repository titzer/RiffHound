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
#include <vector>
#include <algorithm>

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
                           int* out_n, float** out_low)
{
    *out_n = 0;
    if (out_low) *out_low = nullptr;
    if (!pcm || total_frames == 0 || sample_rate == 0 || channels == 0) return nullptr;

    uint64_t s0 = (uint64_t)(t_start * sample_rate);
    uint64_t s1 = (uint64_t)(t_end   * sample_rate);
    if (s1 > total_frames) s1 = total_frames;
    if (s0 >= s1 || s1 - s0 < (uint64_t)BF_FFT) return nullptr;

    int n = (int)((s1 - s0 - BF_FFT) / BF_HOP) + 1;
    if (n <= 0) return nullptr;

    float* flux = (float*)calloc(n, sizeof(float));
    float* lowf = out_low ? (float*)calloc(n, sizeof(float)) : nullptr;
    float* re   = (float*)malloc(BF_FFT * sizeof(float));
    float* im   = (float*)malloc(BF_FFT * sizeof(float));
    float* prev = (float*)calloc(BF_BINS, sizeof(float));
    if (!flux || !re || !im || !prev || (out_low && !lowf)) {
        free(flux); free(lowf); free(re); free(im); free(prev); return nullptr;
    }
    // "Low" = bins below ~260 Hz: kick and bass, the onsets that sit on the
    // beat (not the hats and snares that sit between).
    int low_bins = (int)(260.0 * BF_FFT / sample_rate) + 1;
    if (low_bins > BF_BINS) low_bins = BF_BINS;

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
        float sf = 0.0f, sl = 0.0f;
        for (int b = 0; b < BF_BINS; b++) {
            float mag  = sqrtf(re[b]*re[b] + im[b]*im[b]);
            float diff = mag - prev[b];
            if (diff > 0.0f) { sf += diff; if (b < low_bins) sl += diff; }
            prev[b] = mag;
        }
        flux[f] = sf;
        if (lowf) lowf[f] = sl;
    }

    free(re); free(im); free(prev);

    // Normalise the ODF to unit standard deviation: the DP's tightness and
    // the grid-bias amplitude are meaningful only relative to the ODF scale,
    // which otherwise varies by orders of magnitude with the recording level.
    {
        double mean = 0.0;
        for (int i = 0; i < n; i++) mean += flux[i];
        mean /= n;
        double var = 0.0;
        for (int i = 0; i < n; i++) { double d = flux[i] - mean; var += d * d; }
        float inv = var > 1e-12 ? (float)(1.0 / sqrt(var / n)) : 1.0f;
        for (int i = 0; i < n; i++) flux[i] *= inv;
        if (lowf) for (int i = 0; i < n; i++) lowf[i] *= inv;
    }
    *out_n = n;
    if (out_low) *out_low = lowf;
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
static float estimate_period(const float* flux, const float* lowflux, int n,
                              float min_bpm, float max_bpm,
                              uint32_t sample_rate)
{
    float fps    = (float)sample_rate / BF_HOP;   // ODF frames per second
    int lag_min  = (int)(fps * 60.0f / max_bpm);
    int lag_max  = (int)(fps * 60.0f / min_bpm);
    if (lag_min < 1)    lag_min = 1;
    if (lag_max >= n / 2) lag_max = n / 2 - 1;
    if (lag_min > lag_max) return fps;  // fallback to 1 BPS

    // De-meaned, variance-normalised autocorrelation: the raw product carries
    // a large DC pedestal that flattens the peaks and lets noise pick the
    // argmax.  Computed out to 3*lag_max for the harmonic sum below.
    int ac_max = std::min(n - 1, 3 * lag_max);
    std::vector<float> ac(ac_max + 1, 0.0f);
    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += flux[i];
    mean /= n;
    double var = 0.0;
    for (int i = 0; i < n; i++) { double d = flux[i] - mean; var += d * d; }
    if (var < 1e-12) return fps;
    for (int lag = 1; lag <= ac_max; lag++) {
        double s = 0.0;
        int cnt = n - lag;
        for (int i = 0; i < cnt; i++) s += (flux[i] - mean) * (flux[i + lag] - mean);
        ac[lag] = (float)(s / var);         // ~[-1, 1], relative to full-signal variance
    }
    auto ac_at = [&](float lag) -> float {
        int l0 = (int)lag;
        if (l0 < 1 || l0 + 1 > ac_max) return 0.0f;
        float fr = lag - l0;
        return ac[l0] * (1.0f - fr) + ac[l0 + 1] * fr;
    };

    // The same autocorrelation over the low-band ODF: kick and bass move at
    // the beat, not at the eighth-note subdivision, so their periodicity
    // breaks the octave tie the broadband ODF cannot.
    std::vector<float> acl;
    if (lowflux) {
        double lmean = 0.0;
        for (int i = 0; i < n; i++) lmean += lowflux[i];
        lmean /= n;
        double lvar = 0.0;
        for (int i = 0; i < n; i++) { double d = lowflux[i] - lmean; lvar += d * d; }
        if (lvar > 1e-12) {
            acl.assign(ac_max + 1, 0.0f);
            for (int lag = 1; lag <= ac_max; lag++) {
                double s = 0.0;
                int cnt = n - lag;
                for (int i = 0; i < cnt; i++) s += (lowflux[i] - lmean) * (lowflux[i + lag] - lmean);
                acl[lag] = (float)(s / lvar);
            }
        }
    }
    auto acl_at = [&](float lag) -> float {
        if (acl.empty()) return 0.0f;
        int l0 = (int)lag;
        if (l0 < 1 || l0 + 1 > ac_max) return 0.0f;
        float fr = lag - l0;
        return acl[l0] * (1.0f - fr) + acl[l0 + 1] * fr;
    };

    // Harmonic scoring with a mild log-Gaussian tempo prior (centre 120 BPM):
    // the true beat period is supported by its own multiples, a 1.5x lock is
    // not, and the prior discourages eighth-note and half-time locks when the
    // evidence is ambiguous.
    float best_val = -1e9f;
    float best_lag = (float)lag_min;
    for (int lag = lag_min; lag <= lag_max; lag++) {
        float sec   = (float)lag / fps;
        float lg    = log2f(sec / 0.6f);
        float prior = expf(-0.5f * lg * lg / (1.4f * 1.4f));
        float val   = prior * (ac_at((float)lag) + 0.5f * ac_at(2.0f * lag)
                               + 0.33f * ac_at(3.0f * lag)
                               + acl_at((float)lag) + 0.5f * acl_at(2.0f * lag));
        if (val > best_val) { best_val = val; best_lag = (float)lag; }
    }
    if (getenv("TEMPO_DEBUG")) {
        for (float mul : { 0.5f, 2.0f/3.0f, 1.0f, 4.0f/3.0f, 1.5f, 2.0f }) {
            float lag = best_lag * mul;
            if (lag < 1 || lag > (float)lag_max) continue;
            float sec = lag / fps;
            float lg  = log2f(sec / 0.6f);
            fprintf(stderr, "  [tempo] %6.1f BPM  ac %.3f ac2 %.3f ac3 %.3f  low %.3f low2 %.3f  prior %.2f\n",
                    60.0f / sec, ac_at(lag), ac_at(2 * lag), ac_at(3 * lag),
                    acl_at(lag), acl_at(2 * lag), expf(-0.5f * lg * lg / (1.4f * 1.4f)));
        }
    }
    return best_lag;
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
    float tight    = (params->dp_tightness   > 0.0f) ? params->dp_tightness   : 50.0f;
    double pre_sec = (double)params->pre_onset_ms / 1000.0;

    // 1. Spectral flux ODF (plus a low-band ODF for downbeat-phase checks)
    int    n_flux = 0;
    float* lowflux = nullptr;
    float* flux   = compute_flux(pcm, frame_count, channels, sample_rate,
                                 t_start, t_end, &n_flux, &lowflux);
    if (!flux || n_flux < 4) { free(flux); free(lowflux); return; }

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
                           : estimate_period(flux, lowflux, n_flux, min_bpm, max_bpm, sample_rate);
    } else {
        tau = estimate_period(flux, lowflux, n_flux, min_bpm, max_bpm, sample_rate);
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
        if (getenv("BM_NO_GRID_BIAS")) steadiness = 0.0f;
        if (steadiness > 0.01f) {
            float flux_mean = 0.0f;
            for (int f = 0; f < n_flux; f++) flux_mean += flux[f];
            if (n_flux > 0) flux_mean /= n_flux;

            float amplitude = steadiness * 4.0f * flux_mean;
            for (int f = 0; f < n_flux; f++) {
                double t = t_start + (double)f * hop_sec;
                double phase_sec = fmod(t - seed_anchor, tau_sec);
                if (phase_sec < 0.0) phase_sec += tau_sec;
                // The seeds' phase is only trustworthy near the seeds: a
                // constant-phase cosine extrapolated minutes past the mapped
                // region drifts against the real tempo and then *fights* the
                // audio, planting the fill half a beat off.  Full strength
                // within ~4 beats of a seed, fading to nothing by ~30.
                double dist = 1e18;
                for (int j = 0; j < ns; j++) {
                    double d = fabs(seeds[j] - t);
                    if (d < dist) dist = d;
                }
                double d_beats = dist / tau_sec;
                float  taper = d_beats <= 4.0 ? 1.0f
                             : (float)exp(-(d_beats - 4.0) / 10.0);
                float bias = taper * amplitude * cosf(2.0f * 3.14159265f *
                                              (float)(phase_sec / tau_sec));
                flux[f] = fmaxf(0.0f, flux[f] + bias);
            }
        }
    }

    // 4. DP beat tracking on the (possibly biased) ODF
    int beat_frames[MAX_BEAT_CANDS];
    int beat_count  = 0;
    dp_beat_track(flux, n_flux, tau, tight, beat_frames, &beat_count, MAX_BEAT_CANDS);

    if (beat_count == 0) { free(flux); free(lowflux); return; }

    free(flux);
    flux = nullptr;

    // 4.5. Downbeat-phase check (unseeded only: seeds carry the phase).  The
    // DP follows the loudest periodic onsets, which on backbeat-heavy tracks
    // are the off-beats.  Kick and bass sit on the beat: if the half-period-
    // shifted grid collects clearly more low-band flux, flip the phase.
    if ((params->seed_count < 2 || !params->seed_times) && lowflux && beat_count >= 8) {
        int half = (int)(tau * 0.5f + 0.5f);
        auto low_at = [&](int f) -> float {
            float best = 0.0f;
            for (int d = -1; d <= 1; d++) {
                int i = f + d;
                if (i >= 0 && i < n_flux && lowflux[i] > best) best = lowflux[i];
            }
            return best;
        };
        float on = 0.0f, shifted = 0.0f;
        for (int i = 0; i < beat_count; i++) {
            on      += low_at(beat_frames[i]);
            shifted += low_at(beat_frames[i] + half);
        }
        if (shifted > 1.3f * on) {
            for (int i = 0; i < beat_count; i++) {
                beat_frames[i] += half;
                if (beat_frames[i] >= n_flux) beat_frames[i] = n_flux - 1;
            }
        }
    }
    free(lowflux);
    lowflux = nullptr;

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
