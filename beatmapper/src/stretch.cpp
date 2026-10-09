#include "stretch.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const double PI = 3.14159265358979323846;

// ---- tuning -----------------------------------------------------------------
//
// Chosen with an offline harness that renders clips through the engine and
// scores log-band spectra (long and short frames) against the input
// resampled by the pitch ratio.  A 3072-frame window (70 ms at 44.1 kHz)
// zero-padded to a 4096-point FFT keeps bass partials resolved (the long-frame score) while smearing attacks
// less than a full-length window (the short-frame score); 8x overlap scored
// no better than 4x at twice the cost.

#define OVERLAP       4     // window / synthesis hop
#define KW           16     // resampler kernel half-width, in zero crossings
#define KRES        512     // kernel table entries per zero crossing
#define KBETA       9.0     // Kaiser beta for the kernel (~-90 dB sidelobes)
#define RING         64     // hop history (power of two)
#define QUIET     1e-10f    // bins this far below the loudest are float noise

// ---- FFT --------------------------------------------------------------------

// Iterative radix-2 complex FFT with precomputed twiddles and bit reversal.
struct Fft {
    int    n;
    float* cs;    // cos(2 pi j / n), j < n/2
    float* sn;    // -sin(2 pi j / n)
    int*   rev;
};

static bool fft_init(Fft* f, int n)
{
    f->n   = n;
    f->cs  = (float*)malloc(sizeof(float) * n / 2);
    f->sn  = (float*)malloc(sizeof(float) * n / 2);
    f->rev = (int*)malloc(sizeof(int) * n);
    if (!f->cs || !f->sn || !f->rev) return false;
    for (int j = 0; j < n / 2; j++) {
        f->cs[j] = (float)cos(2.0 * PI * j / n);
        f->sn[j] = (float)-sin(2.0 * PI * j / n);
    }
    int bits = 0;
    while ((1 << bits) < n) bits++;
    for (int i = 0; i < n; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        f->rev[i] = r;
    }
    return true;
}

static void fft_free(Fft* f)
{
    free(f->cs); free(f->sn); free(f->rev);
    f->cs = f->sn = nullptr; f->rev = nullptr;
}

// Forward, unnormalized, in place.
static void fft_run(const Fft* f, float* re, float* im)
{
    const int n = f->n;
    for (int i = 0; i < n; i++) {
        int j = f->rev[i];
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2, step = n / 2; len <= n; len <<= 1, step >>= 1) {
        int half = len >> 1;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; j++) {
                float wr = f->cs[j * step], wi = f->sn[j * step];
                int a = i + j, b = a + half;
                float vr = re[b] * wr - im[b] * wi;
                float vi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] += vr;        im[a] += vi;
            }
        }
    }
}

// The transform of x + i*y, split into the half spectra (bins 0..n/2) of the
// real signals x and y.
static void fft_unpack(const float* re, const float* im, int n,
                       float* xr, float* xi, float* yr, float* yi)
{
    for (int k = 0; k <= n / 2; k++) {
        int m = (n - k) & (n - 1);
        xr[k] = 0.5f * (re[k] + re[m]);
        xi[k] = 0.5f * (im[k] - im[m]);
        yr[k] = 0.5f * (im[k] + im[m]);
        yi[k] = 0.5f * (re[m] - re[k]);
    }
}

static inline double princarg(double a)
{
    return a - 2.0 * PI * floor(a / (2.0 * PI) + 0.5);
}

// ---- state ------------------------------------------------------------------

struct HopInfo {
    double centre;   // input frame centre of this analysis hop
    float  rate;     // input frames per stretched frame (analysis hop / H)
    float  pitch;    // pitch ratio the hop was made for
};

struct StretchState {
    int N, NB;                 // FFT size, bins (N/2 + 1)
    int W;                     // window length (<= N, zero-padded to N)
    int H;                     // synthesis hop
    int L;                     // synthesis span: W rounded up to a multiple of 2H
    Fft fft;
    float* win;                // Hann of length W centred in N (analysis and synthesis)
    float  ola_gain;           // 1 / sum of overlapping win^2

    // FFT work and spectra
    float *wr, *wi, *vr, *vi;                  // N
    float *lr, *li, *rr, *ri;                  // channel spectra, NB
    float *phase_in, *phase_prev;              // arg of the mono spectrum: now, last hop
    float *pw, *pw_prev;                       // power, both channels: now, last hop
    double* phase_out;                         // mono output phase, last hop
    float *rot;                                // per-bin rotation this hop
    float *hkey; int *hid; unsigned char* done;   // phase integration heap
    float *ola;                                // L * STRETCH_MAX_CH accumulator

    bool    first;             // next hop takes the input phases as they are
    double  apos;              // input frame centre of the next analysis hop
    int64_t prev_c;            // frame centre of the last hop

    // Stretched stream: samples [sb_base, sb_base + sb_len) live in sb.
    float*  sb;
    int     sb_cap, sb_len;
    int64_t sb_base;
    int64_t hops;              // hops made; hop s wrote stretched [s*H, s*H+H)
    HopInfo ring[RING];

    double  rpos;              // stretched position of the next output frame
    float*  ktab;              // windowed sinc, KW*KRES+2 entries
};

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

static void st_free(StretchState* st)
{
    if (!st) return;
    fft_free(&st->fft);
    float* fs[] = { st->win, st->wr, st->wi, st->vr, st->vi,
                    st->lr, st->li, st->rr, st->ri, st->phase_in, st->phase_prev,
                    st->pw, st->pw_prev, st->rot, st->hkey, st->ola, st->sb, st->ktab };
    for (float* p : fs) free(p);
    free(st->phase_out);
    free(st->hid);
    free(st->done);
    free(st);
}

static StretchState* st_alloc(uint32_t sample_rate)
{
    StretchState* st = (StretchState*)calloc(1, sizeof(StretchState));
    if (!st) return nullptr;
    int N = sample_rate > 64000 ? 8192 : 4096;
    int W = N * 3 / 4;
    st->N = N; st->NB = N / 2 + 1; st->W = W; st->H = W / OVERLAP;
    st->L = 2 * st->H * ((W + 2 * st->H - 1) / (2 * st->H));
    bool ok = fft_init(&st->fft, N);
    auto fa = [&](int n) { float* p = (float*)calloc(n, sizeof(float)); ok = ok && p; return p; };
    const int NB = st->NB;
    st->win = fa(N);
    st->wr = fa(N); st->wi = fa(N); st->vr = fa(N); st->vi = fa(N);
    st->lr = fa(NB); st->li = fa(NB); st->rr = fa(NB); st->ri = fa(NB);
    st->phase_in = fa(NB); st->phase_prev = fa(NB);
    st->pw = fa(NB); st->pw_prev = fa(NB); st->rot = fa(NB);
    st->hkey = fa(2 * NB);
    st->ola  = fa(st->L * STRETCH_MAX_CH);
    st->sb_cap = 4 * N;
    st->sb   = fa(st->sb_cap * STRETCH_MAX_CH);
    st->ktab = fa(KW * KRES + 2);
    st->phase_out = (double*)calloc(NB, sizeof(double));
    st->hid  = (int*)calloc(2 * NB, sizeof(int));
    st->done = (unsigned char*)calloc(NB, 1);
    if (!ok || !st->phase_out || !st->hid || !st->done) { st_free(st); return nullptr; }

    for (int i = 0; i < N; i++) {
        int j = i - (N - W) / 2;
        st->win[i] = (j >= 0 && j < W) ? (float)(0.5 - 0.5 * cos(2.0 * PI * j / W)) : 0.0f;
    }
    // Every output sample sees the same sum of squared windows (3/8 * OVERLAP
    // for Hann); measure it half a hop off a window edge.
    double wsum = 0.0;
    for (int i = (N / 2 + st->H / 2) % st->H; i < N; i += st->H)
        wsum += (double)st->win[i] * st->win[i];
    st->ola_gain = (float)(1.0 / wsum);

    double i0b = bessel_i0(KBETA);
    for (int i = 0; i <= KW * KRES + 1; i++) {
        double t = (double)i / KRES;
        double s = (i == 0) ? 1.0 : sin(PI * t) / (PI * t);
        double u = t / KW;
        double w = (u < 1.0) ? bessel_i0(KBETA * sqrt(1.0 - u * u)) / i0b : 0.0;
        st->ktab[i] = (float)(s * w);
    }
    return st;
}

// ---- analysis helpers -------------------------------------------------------

// Window the n frames centred on c into dst, rotated by n/2 so the window
// centre lands on index 0 (zero-phase framing: a steady partial's main-lobe
// bins share one phase, and a bin's phase refers to the frame centre).
// which: 0 = left, 1 = right, 2 = mono sum.  Out-of-range frames read 0.
static void load_frame(const float* pcm, uint64_t frames, uint32_t ch,
                       int64_t c, const float* win, int n, int which, float* dst)
{
    const int half = n / 2;
    for (int i = 0; i < n; i++) {
        int64_t t = c - half + i;
        float v = 0.0f;
        if (win[i] != 0.0f && t >= 0 && (uint64_t)t < frames) {
            const float* s = pcm + t * ch;
            if (which == 2)           v = (ch > 1) ? s[0] + s[1] : s[0];
            else if (which < (int)ch) v = s[which];
        }
        dst[(i + half) & (n - 1)] = v * win[i];
    }
}

// ---- phase integration ------------------------------------------------------

// Max-heap of (power, id); id = bin, + NB for a bin of the previous hop.
struct Heap { float* key; int* id; int n; };

static void heap_push(Heap& h, float k, int id)
{
    int i = h.n++;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (h.key[p] >= k) break;
        h.key[i] = h.key[p]; h.id[i] = h.id[p]; i = p;
    }
    h.key[i] = k; h.id[i] = id;
}

static int heap_pop(Heap& h)
{
    int top = h.id[0];
    float k = h.key[--h.n]; int id = h.id[h.n];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= h.n) break;
        if (c + 1 < h.n && h.key[c + 1] > h.key[c]) c++;
        if (h.key[c] <= k) break;
        h.key[i] = h.key[c]; h.id[i] = h.id[c]; i = c;
    }
    if (h.n > 0) { h.key[i] = k; h.id[i] = id; }
    return top;
}

// Phase-gradient heap integration (after Prusa & Holighaus, "Phase Vocoder
// Done Right", 2017), in an identity-preserving form.  Bins are visited
// loudest first.  A bin reached from the previous hop advances its output
// phase by its instantaneous frequency times the synthesis hop; a bin reached
// from a louder neighbour in this hop keeps the input's phase difference to
// that neighbour.  So steady partials stay continuous in time, main lobes
// and the bins of an attack stay vertically coherent, and transients need
// no detector: in an attack frame the new bins outweigh the old ones.
// lag_r/lag_i is the mono spectrum one synthesis hop before the current
// frame (nullptr: it is the previous hop's, so phase_prev serves).
// Writes the per-bin rotation (output minus input phase) into st->rot.
static void integrate_phases(StretchState* st, const float* lag_r, const float* lag_i)
{
    const int N = st->N, H = st->H, NB = st->NB;
    float pmax = 0.0f;
    for (int k = 0; k < NB; k++) if (st->pw[k] > pmax) pmax = st->pw[k];
    // Bins in float rounding noise keep their input phase; integrating them
    // would hand random phases to partials that arrive there later.
    const float tol = fmaxf(pmax * QUIET, 1e-12f);

    Heap h = { st->hkey, st->hid, 0 };
    int open = 0;
    for (int k = 0; k < NB; k++) {
        st->rot[k] = 0.0f;
        bool quiet = st->first || st->pw[k] <= tol;
        st->done[k] = quiet;
        if (!quiet) open++;
        if (!st->first && st->pw_prev[k] > tol) heap_push(h, st->pw_prev[k], k + NB);
    }
    while (open > 0) {
        if (h.n == 0) {
            // Nothing to inherit from: the loudest open bin keeps its input phase.
            int best = -1;
            for (int k = 0; k < NB; k++)
                if (!st->done[k] && (best < 0 || st->pw[k] > st->pw[best])) best = k;
            st->done[best] = 1; open--;
            heap_push(h, st->pw[best], best);
            continue;
        }
        int id = heap_pop(h);
        if (id >= NB) {
            int k = id - NB;
            if (st->done[k]) continue;
            double omega_k = 2.0 * PI * k / N;
            double phase_a = lag_r ? atan2((double)lag_i[k], (double)lag_r[k])
                                   : (double)st->phase_prev[k];
            double omega   = omega_k + princarg(st->phase_in[k] - phase_a - omega_k * H) / H;
            st->rot[k] = (float)princarg(st->phase_out[k] + omega * H - st->phase_in[k]);
            st->done[k] = 1; open--;
            heap_push(h, st->pw[k], k);
        } else {
            for (int j = id - 1; j <= id + 1; j += 2) {
                if (j < 0 || j >= NB || st->done[j]) continue;
                st->rot[j] = st->rot[id];
                st->done[j] = 1; open--;
                heap_push(h, st->pw[j], j);
            }
        }
    }
}

// ---- phase vocoder hop ------------------------------------------------------

// Produce one synthesis hop (H stretched frames) and append it to sb.
static void pv_hop(StretchSource* ss)
{
    StretchState* st = ss->st;
    const int N = st->N, H = st->H, NB = st->NB, L = st->L;
    const uint32_t ch = ss->channels;
    const float* pcm = ss->override_active ? ss->override_active : ss->pcm;
    const uint64_t frames = ss->frame_count;

    float speed = ss->speed.load(std::memory_order_relaxed);
    float pitch = ss->pitch.load(std::memory_order_relaxed);
    if (pitch < 0.25f) pitch = 0.25f;
    if (pitch > 4.0f)  pitch = 4.0f;
    if (speed < 0.05f) speed = 0.05f;
    const double ha = (double)H * speed / pitch;   // analysis hop

    // Loop wrap: jump the analysis position back and restart the phases;
    // the overlapping frames crossfade loop end into loop start.
    bool     loop_en = ss->loop_enabled.load(std::memory_order_relaxed);
    uint64_t loop_s  = ss->loop_start_frames.load(std::memory_order_relaxed);
    uint64_t loop_e  = ss->loop_end_frames.load(std::memory_order_relaxed);
    if (loop_en && loop_e > loop_s && st->apos >= (double)loop_e) {
        double range = (double)(loop_e - loop_s);
        st->apos  = (double)loop_s + fmod(st->apos - (double)loop_s, range);
        st->first = true;
    }

    const int64_t c = (int64_t)floor(st->apos + 0.5);
    HopInfo& hi = st->ring[st->hops & (RING - 1)];
    hi.centre = st->apos;
    hi.rate   = (float)(ha / H);
    hi.pitch  = pitch;

    // Both channels at c in one packed FFT.
    load_frame(pcm, frames, ch, c, st->win, N, 0, st->wr);
    load_frame(pcm, frames, ch, c, st->win, N, 1, st->wi);
    fft_run(&st->fft, st->wr, st->wi);
    fft_unpack(st->wr, st->wi, N, st->lr, st->li, st->rr, st->ri);
    for (int k = 0; k < NB; k++) {
        float mr = st->lr[k] + st->rr[k], mi = st->li[k] + st->ri[k];
        st->phase_in[k] = atan2f(mi, mr);
        st->pw[k] = st->lr[k] * st->lr[k] + st->li[k] * st->li[k]
                  + st->rr[k] * st->rr[k] + st->ri[k] * st->ri[k];
    }

    // Instantaneous frequencies come from the phase change over one
    // synthesis hop.  Unless the analysis hop equals it (no stretch), that
    // takes the mono spectrum at c - H as well.
    if (st->first) {
        integrate_phases(st, nullptr, nullptr);
    } else if (c - H == st->prev_c) {
        integrate_phases(st, nullptr, nullptr);
    } else {
        load_frame(pcm, frames, ch, c - H, st->win, N, 2, st->vr);
        memset(st->vi, 0, N * sizeof(float));
        fft_run(&st->fft, st->vr, st->vi);
        // Bins 0..N/2 of a real signal's transform are its half spectrum.
        integrate_phases(st, st->vr, st->vi);
    }
    st->rot[0] = st->rot[NB - 1] = 0.0f;   // DC and Nyquist must stay real

    // Rotate both channels by the same angles (so the stereo image holds),
    // remember the mono output phase, and pack the two real-signal spectra
    // into one complex spectrum for a single inverse FFT.
    for (int k = 0; k < NB; k++) {
        float cr = cosf(st->rot[k]), ci = sinf(st->rot[k]);
        float l_r = st->lr[k] * cr - st->li[k] * ci, l_i = st->lr[k] * ci + st->li[k] * cr;
        float r_r = st->rr[k] * cr - st->ri[k] * ci, r_i = st->rr[k] * ci + st->ri[k] * cr;
        st->phase_out[k] = princarg((double)st->phase_in[k] + st->rot[k]);
        // Z[k] = L + iR, conjugated for an inverse transform via the forward one.
        st->wr[k] = l_r - r_i;
        st->wi[k] = -(l_i + r_r);
        if (k > 0 && k < NB - 1) {
            // Z[N-k] = conj(L) + i conj(R), conjugated.
            st->wr[N - k] = l_r + r_i;
            st->wi[N - k] = l_i - r_r;
        }
    }
    fft_run(&st->fft, st->wr, st->wi);
    // Inverse = conj(fft(conj Z)) / N: real part is left, -imag part is right.
    // Overlap-add the window's span, centred in L.
    const float g = st->ola_gain / N;
    const int half = N / 2;
    for (int i = 0; i < L; i++) {
        int n = i - L / 2 + half;
        int j = (n + half) & (N - 1);
        float w = st->win[n] * g;
        st->ola[i * STRETCH_MAX_CH + 0] += st->wr[j] * w;
        st->ola[i * STRETCH_MAX_CH + 1] -= st->wi[j] * w;
    }

    // Emit the first H frames into the stretched stream.
    if (st->sb_len + H > st->sb_cap) {
        // Keep enough history behind rpos for the resampler kernel.
        int64_t keep_from = (int64_t)floor(st->rpos) - (int64_t)(KW * 4 + 8);
        int drop = (int)(keep_from - st->sb_base);
        if (drop > st->sb_len) drop = st->sb_len;
        if (drop > 0) {
            memmove(st->sb, st->sb + drop * STRETCH_MAX_CH,
                    (st->sb_len - drop) * STRETCH_MAX_CH * sizeof(float));
            st->sb_len  -= drop;
            st->sb_base += drop;
        }
    }
    memcpy(st->sb + st->sb_len * STRETCH_MAX_CH, st->ola, H * STRETCH_MAX_CH * sizeof(float));
    st->sb_len += H;
    memmove(st->ola, st->ola + H * STRETCH_MAX_CH, (L - H) * STRETCH_MAX_CH * sizeof(float));
    memset(st->ola + (L - H) * STRETCH_MAX_CH, 0, H * STRETCH_MAX_CH * sizeof(float));

    float* t;
    t = st->pw_prev;    st->pw_prev    = st->pw;       st->pw       = t;
    t = st->phase_prev; st->phase_prev = st->phase_in; st->phase_in = t;
    st->prev_c = c;
    st->apos  += ha;
    st->hops++;
    st->first  = false;
}

// The hop whose frame is centred on stretched position x: L/2 behind the
// hop that emitted x.
static const HopInfo& hop_at(const StretchState* st, double x)
{
    int64_t b = (int64_t)floor(x / st->H);
    return st->ring[(b - st->L / (2 * st->H)) & (RING - 1)];
}

// Input frame that stretched position x was made from.
static double input_time(const StretchState* st, double x)
{
    int64_t b = (int64_t)floor(x / st->H);
    const HopInfo& hi = hop_at(st, x);
    return hi.centre + (x - (double)b * st->H) * hi.rate;
}

// Restart the pipeline so the first output frame is input frame `frame`.
static void st_reset(StretchSource* ss, uint64_t frame)
{
    StretchState* st = ss->st;
    float speed = ss->speed.load(std::memory_order_relaxed);
    float pitch = ss->pitch.load(std::memory_order_relaxed);
    if (pitch < 0.25f) pitch = 0.25f;
    if (pitch > 4.0f)  pitch = 4.0f;
    double ha = (double)st->H * speed / pitch;
    // Output starts at the first hop that every overlapping frame reaches;
    // it shows the frame of the hop `lag` earlier.
    const int prime = st->L / st->H - 1, lag = st->L / (2 * st->H);
    st->apos    = (double)frame - (prime - lag) * ha;
    st->prev_c  = INT64_MIN;
    st->hops    = 0;
    st->sb_base = 0;
    st->sb_len  = 0;
    st->rpos    = (double)prime * st->H;
    st->first   = true;
    memset(st->ola, 0, sizeof(float) * st->L * STRETCH_MAX_CH);
    memset(st->ring, 0, sizeof(st->ring));
    ss->cursor_frames.store(frame, std::memory_order_relaxed);
}

// ---- vtable callbacks -------------------------------------------------------

static ma_result stretch_on_read(ma_data_source* pDS, void* pFramesOut,
                                 ma_uint64 frameCount, ma_uint64* pFramesRead);
static ma_result stretch_on_seek(ma_data_source* pDS, ma_uint64 frameIndex);
static ma_result stretch_on_get_data_format(ma_data_source* pDS, ma_format* pFormat,
                                            ma_uint32* pChannels, ma_uint32* pSampleRate,
                                            ma_channel* pChannelMap, size_t channelMapCap);
static ma_result stretch_on_get_cursor(ma_data_source* pDS, ma_uint64* pCursor);
static ma_result stretch_on_get_length(ma_data_source* pDS, ma_uint64* pLength);

static ma_data_source_vtable s_vtable = {
    stretch_on_read,
    stretch_on_seek,
    stretch_on_get_data_format,
    stretch_on_get_cursor,
    stretch_on_get_length,
    NULL,   // onSetLooping
    0,      // flags
};

// Sentinel posted in override_pending to mean "back to the original pcm".
static float s_override_none;

static ma_result stretch_on_read(ma_data_source* pDS, void* pFramesOut,
                                 ma_uint64 frameCount, ma_uint64* pFramesRead)
{
    StretchSource* ss = (StretchSource*)pDS;
    StretchState*  st = ss->st;
    float*         out = (float*)pFramesOut;
    const uint32_t ch  = ss->channels;

    // Adopt a posted override, once the previous one has been collected.
    if (ss->override_retired.load(std::memory_order_relaxed) == nullptr) {
        float* p = ss->override_pending.exchange(nullptr, std::memory_order_acquire);
        if (p) {
            ss->override_retired.store(ss->override_active, std::memory_order_release);
            ss->override_active = (p == &s_override_none) ? nullptr : p;
        }
    }

    const bool looping = ss->loop_enabled.load(std::memory_order_relaxed) &&
                         ss->loop_end_frames.load(std::memory_order_relaxed) >
                         ss->loop_start_frames.load(std::memory_order_relaxed);
    const double eof = (double)ss->frame_count;
    ma_uint64 written = 0;
    while (written < frameCount) {
        // Synthesize until the kernel's reach ahead of rpos exists.  The
        // pitch (resampling step) is the one the frame under rpos was made for.
        float pitch, fc;
        int reach;
        for (;;) {
            int64_t made = st->hops * st->H, at = (int64_t)floor(st->rpos);
            if (made > at + st->H) {
                pitch = hop_at(st, st->rpos).pitch;
                // Cutoff at the new Nyquist when pitching up, so nothing aliases.
                fc = (fabsf(pitch - 1.0f) < 1e-6f) ? 1.0f : fminf(1.0f, 0.97f / pitch);
                reach = (int)ceilf(KW / fc) + 1;
                if (made > at + reach) break;
            }
            pv_hop(ss);
        }
        if (!looping && input_time(st, st->rpos) >= eof) break;

        // Windowed-sinc interpolation at rpos (gain normalized to 1).
        int64_t i0 = (int64_t)floor(st->rpos);
        float frac = (float)(st->rpos - (double)i0);
        float acc0 = 0.0f, acc1 = 0.0f, wsum = 0.0f;
        const float scale = fc * KRES;
        const float* base = st->sb + (i0 - st->sb_base) * STRETCH_MAX_CH;
        for (int j = -reach + 1; j <= reach; j++) {
            float t = fabsf((float)j - frac) * scale;
            int   ti = (int)t;
            if (ti >= KW * KRES) continue;
            float w = st->ktab[ti] + (st->ktab[ti + 1] - st->ktab[ti]) * (t - ti);
            const float* s = base + j * STRETCH_MAX_CH;
            acc0 += s[0] * w;
            acc1 += s[1] * w;
            wsum += w;
        }
        acc0 /= wsum; acc1 /= wsum;
        out[written * ch] = acc0;
        if (ch > 1) out[written * ch + 1] = acc1;
        written++;
        st->rpos += pitch;
    }

    double cur = input_time(st, st->rpos);
    ss->cursor_frames.store(cur > 0.0 ? (uint64_t)cur : 0, std::memory_order_relaxed);
    if (pFramesRead) *pFramesRead = written;
    return (written < frameCount) ? MA_AT_END : MA_SUCCESS;
}

static ma_result stretch_on_seek(ma_data_source* pDS, ma_uint64 frameIndex)
{
    st_reset((StretchSource*)pDS, frameIndex);
    return MA_SUCCESS;
}

static ma_result stretch_on_get_data_format(ma_data_source* pDS, ma_format* pFormat,
                                            ma_uint32* pChannels, ma_uint32* pSampleRate,
                                            ma_channel* pChannelMap, size_t channelMapCap)
{
    StretchSource* ss = (StretchSource*)pDS;
    if (pFormat)     *pFormat     = ma_format_f32;
    if (pChannels)   *pChannels   = ss->channels;
    if (pSampleRate) *pSampleRate = ss->sample_rate;
    if (pChannelMap)
        ma_channel_map_init_standard(ma_standard_channel_map_default,
                                     pChannelMap, channelMapCap, ss->channels);
    return MA_SUCCESS;
}

static ma_result stretch_on_get_cursor(ma_data_source* pDS, ma_uint64* pCursor)
{
    StretchSource* ss = (StretchSource*)pDS;
    *pCursor = ss->cursor_frames.load(std::memory_order_relaxed);
    return MA_SUCCESS;
}

static ma_result stretch_on_get_length(ma_data_source* pDS, ma_uint64* pLength)
{
    StretchSource* ss = (StretchSource*)pDS;
    *pLength = ss->frame_count;
    return MA_SUCCESS;
}

// ---- public API -------------------------------------------------------------

bool stretch_init(StretchSource* ss, float* pcm, uint64_t frames,
                  uint32_t channels, uint32_t sample_rate, bool owns_pcm)
{
    // Zero only the POD members -- memset on a struct with std::atomic members is UB.
    memset(&ss->base, 0, sizeof(ss->base));
    ss->pcm            = nullptr;
    ss->frame_count    = 0;
    ss->channels       = 0;
    ss->sample_rate    = 0;
    ss->owns_pcm       = false;
    ss->override_pending.store(nullptr, std::memory_order_relaxed);
    ss->override_active = nullptr;
    ss->override_retired.store(nullptr, std::memory_order_relaxed);
    ss->speed.store(1.0f, std::memory_order_relaxed);
    ss->pitch.store(1.0f, std::memory_order_relaxed);
    ss->cursor_frames.store(0, std::memory_order_relaxed);
    ss->loop_enabled.store(false, std::memory_order_relaxed);
    ss->loop_start_frames.store(0, std::memory_order_relaxed);
    ss->loop_end_frames.store(0, std::memory_order_relaxed);
    ss->st = st_alloc(sample_rate);
    if (!ss->st) return false;

    ma_data_source_config cfg = ma_data_source_config_init();
    cfg.vtable = &s_vtable;
    if (ma_data_source_init(&cfg, &ss->base) != MA_SUCCESS) {
        st_free(ss->st); ss->st = nullptr;
        return false;
    }

    ss->pcm         = pcm;
    ss->frame_count = frames;
    ss->channels    = (channels > STRETCH_MAX_CH) ? STRETCH_MAX_CH : channels;
    ss->sample_rate = sample_rate;
    ss->owns_pcm    = owns_pcm;
    st_reset(ss, 0);
    return true;
}

void stretch_uninit(StretchSource* ss)
{
    ma_data_source_uninit(&ss->base);
    if (ss->owns_pcm && ss->pcm) {
        free(ss->pcm);
        ss->pcm = nullptr;
    }
    // The sound is gone by now: no audio-thread reader remains.
    float* p = ss->override_pending.exchange(nullptr);
    if (p && p != &s_override_none) free(p);
    free(ss->override_active); ss->override_active = nullptr;
    free(ss->override_retired.exchange(nullptr));
    st_free(ss->st); ss->st = nullptr;
}

void stretch_set_override(StretchSource* ss, float* pcm)
{
    float* post = pcm ? pcm : &s_override_none;
    float* prev = ss->override_pending.exchange(post, std::memory_order_acq_rel);
    if (prev && prev != &s_override_none) free(prev);   // never reached the audio thread
}

void stretch_collect_retired(StretchSource* ss)
{
    float* r = ss->override_retired.exchange(nullptr, std::memory_order_acq_rel);
    if (r) {
        if (getenv("BM_AUDIO_DEBUG")) fprintf(stderr, "[audio] override adopted; previous freed\n");
        free(r);
    }
}

void stretch_set_speed(StretchSource* ss, float speed)
{
    ss->speed.store(speed, std::memory_order_relaxed);
}

float stretch_get_speed(const StretchSource* ss)
{
    return ss->speed.load(std::memory_order_relaxed);
}

void stretch_set_pitch(StretchSource* ss, float pitch)
{
    ss->pitch.store(pitch, std::memory_order_relaxed);
}

float stretch_get_pitch(const StretchSource* ss)
{
    return ss->pitch.load(std::memory_order_relaxed);
}

void stretch_set_loop(StretchSource* ss, bool enabled,
                      uint64_t loop_start_frames, uint64_t loop_end_frames)
{
    ss->loop_start_frames.store(loop_start_frames, std::memory_order_relaxed);
    ss->loop_end_frames.store(loop_end_frames,     std::memory_order_relaxed);
    ss->loop_enabled.store(enabled,                std::memory_order_relaxed);
}
