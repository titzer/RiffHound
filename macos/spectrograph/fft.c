/* fft.c – real-valued FFT pipeline implementation */
#include "fft.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Automatic backend selection ─────────────────────────────────────────── *
 * Define FFT_BACKEND_ACCELERATE explicitly to force that path, or let the
 * platform choose.  Add new #elif branches here for additional backends
 * (e.g. FFT_BACKEND_FFTW, FFT_BACKEND_KISSFFT, …).                        */
#if !defined(FFT_BACKEND_ACCELERATE)
#  if defined(__APPLE__)
#    define FFT_BACKEND_ACCELERATE
#  endif
#endif

#if !defined(FFT_BACKEND_ACCELERATE)
#  error "No FFT backend selected. On Apple platforms FFT_BACKEND_ACCELERATE " \
         "is chosen automatically; define it explicitly or add a new backend."
#endif

/* ── Global parameters ───────────────────────────────────────────────────── */
int           fft_size        = 4096;           /* ~93 ms at 44100 Hz */
int           fft_hop_size    = 1024;           /* 75 % overlap       */
FftWindowType fft_window_type = FFT_WINDOW_HANN;
double        fft_sample_rate = 44100.0;

/* ── Read-only derived values ────────────────────────────────────────────── */
int    fft_bin_count       = 0;
double fft_freq_resolution = 0.0;

/* ── Shared internal state ───────────────────────────────────────────────── */
static float *s_ring_buf   = NULL;  /* circular sample buffer (fft_size)    */
static int    s_ring_write = 0;     /* next write position                  */
static int    s_ring_fill  = 0;     /* samples present (saturates at N)     */
static int    s_until_hop  = 0;     /* samples until next FFT trigger       */
static float *s_work_buf   = NULL;  /* linearised + windowed frame (N)      */
static float *s_window     = NULL;  /* precomputed window coefficients (N)  */
static float *s_magnitude  = NULL;  /* output magnitudes (fft_bin_count)    */
static int    s_initialized = 0;

/* ════════════════════════════════════════════════════════════════════════════
 * Accelerate / vDSP backend
 * ════════════════════════════════════════════════════════════════════════════ */
#if defined(FFT_BACKEND_ACCELERATE)
#include <Accelerate/Accelerate.h>

static FFTSetup        s_fft_setup = NULL;
static DSPSplitComplex s_split     = { NULL, NULL };
static vDSP_Length     s_log2n     = 0;

static void impl_destroy(void) {
    if (s_fft_setup) { vDSP_destroy_fftsetup(s_fft_setup); s_fft_setup = NULL; }
    free(s_split.realp); s_split.realp = NULL;
    free(s_split.imagp); s_split.imagp = NULL;
}

static int impl_init(void) {
    /* Compute log2(fft_size).  Caller guarantees fft_size is a power of two. */
    s_log2n = 0;
    for (vDSP_Length n = (vDSP_Length)fft_size; n > 1; n >>= 1)
        s_log2n++;

    s_fft_setup = vDSP_create_fftsetup(s_log2n, kFFTRadix2);
    if (!s_fft_setup) return -1;

    int half = fft_size / 2;
    s_split.realp = (float *)malloc((size_t)half * sizeof(float));
    s_split.imagp = (float *)malloc((size_t)half * sizeof(float));
    if (!s_split.realp || !s_split.imagp) { impl_destroy(); return -1; }
    return 0;
}

static void impl_compute(const float *windowed) {
    /* Pack real input into split-complex by treating adjacent pairs as Re/Im. */
    vDSP_ctoz((const DSPComplex *)windowed, 2,
               &s_split, 1, (vDSP_Length)(fft_size / 2));

    /* In-place forward real FFT.  Output packed format:
         realp[0]  – DC amplitude      (purely real)
         imagp[0]  – Nyquist amplitude (purely real, stored in Im slot)
         realp[k] / imagp[k]  for k = 1 .. N/2-1                       */
    vDSP_fft_zrip(s_fft_setup, &s_split, 1, s_log2n, kFFTDirection_Forward);

    /* DC and Nyquist: purely real in this representation. */
    s_magnitude[0]          = fabsf(s_split.realp[0]);
    s_magnitude[fft_size/2] = fabsf(s_split.imagp[0]);

    /* Intermediate bins: complex magnitude via vDSP_zvabs. */
    DSPSplitComplex mid = { s_split.realp + 1, s_split.imagp + 1 };
    vDSP_zvabs(&mid, 1,
               s_magnitude + 1, 1,
               (vDSP_Length)(fft_size / 2 - 1));

    /* Normalise: vDSP real FFT is 2× the mathematical DFT convention;
       dividing by N/2 makes a full-scale sine at one bin → amplitude ≈ 1. */
    float scale = 2.0f / (float)fft_size;
    vDSP_vsmul(s_magnitude, 1, &scale,
               s_magnitude, 1, (vDSP_Length)fft_bin_count);
}

static void impl_build_window(void) {
    switch (fft_window_type) {
        case FFT_WINDOW_HANN:
            /* vDSP_HANN_DENORM: periodic Hann, peak = 1.0 */
            vDSP_hann_window(s_window, (vDSP_Length)fft_size, vDSP_HANN_DENORM);
            break;
        case FFT_WINDOW_HAMMING:
            vDSP_hamm_window(s_window, (vDSP_Length)fft_size, 0);
            break;
        case FFT_WINDOW_BLACKMAN:
            vDSP_blkman_window(s_window, (vDSP_Length)fft_size, 0);
            break;
        case FFT_WINDOW_RECT:
        default:
            for (int i = 0; i < fft_size; i++) s_window[i] = 1.0f;
            break;
    }
}

#endif /* FFT_BACKEND_ACCELERATE */

/* ── Shared helpers ──────────────────────────────────────────────────────── */

/* Linearise the ring buffer (oldest → newest) into s_work_buf and window it. */
static void prepare_frame(void) {
    int start = s_ring_write;   /* oldest sample is at the current write index */
    for (int i = 0; i < fft_size; i++) {
        int idx = (start + i) % fft_size;
        s_work_buf[i] = s_ring_buf[idx] * s_window[i];
    }
}

static void shared_destroy(void) {
    free(s_ring_buf);  s_ring_buf  = NULL;
    free(s_work_buf);  s_work_buf  = NULL;
    free(s_window);    s_window    = NULL;
    free(s_magnitude); s_magnitude = NULL;
}

static int shared_init(void) {
    fft_bin_count       = fft_size / 2 + 1;
    fft_freq_resolution = fft_sample_rate / (double)fft_size;

    s_ring_buf  = (float *)calloc((size_t)fft_size,       sizeof(float));
    s_work_buf  = (float *)malloc((size_t)fft_size       * sizeof(float));
    s_window    = (float *)malloc((size_t)fft_size       * sizeof(float));
    s_magnitude = (float *)calloc((size_t)fft_bin_count,  sizeof(float));

    if (!s_ring_buf || !s_work_buf || !s_window || !s_magnitude) {
        shared_destroy();
        return -1;
    }

    s_ring_write = 0;
    s_ring_fill  = 0;
    s_until_hop  = fft_hop_size;

    impl_build_window();
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int fft_init(void) {
    if (s_initialized) fft_destroy();
    if (shared_init() != 0) return -1;
    if (impl_init()   != 0) { shared_destroy(); return -1; }
    s_initialized = 1;
    return 0;
}

int fft_reinit(void) { return fft_init(); }

const float *fft_push(const float *samples, int count) {
    if (!s_initialized) return NULL;

    const float *result = NULL;
    int i = 0;

    while (i < count) {
        /* Consume up to s_until_hop samples before the next FFT trigger. */
        int take = (s_until_hop < count - i) ? s_until_hop : (count - i);

        /* Write into the circular ring buffer. */
        for (int j = 0; j < take; j++) {
            s_ring_buf[s_ring_write] = samples[i + j];
            s_ring_write = (s_ring_write + 1) % fft_size;
        }
        s_ring_fill += take;
        if (s_ring_fill > fft_size) s_ring_fill = fft_size;

        s_until_hop -= take;
        i           += take;

        if (s_until_hop == 0) {
            /* Wait until the ring buffer holds a full frame before computing. */
            if (s_ring_fill >= fft_size) {
                prepare_frame();
                impl_compute(s_work_buf);
                result = s_magnitude;
            }
            s_until_hop = fft_hop_size;
        }
    }

    return result;  /* pointer to the most recently computed frame, or NULL */
}

const float *fft_output(void) {
    return s_initialized ? s_magnitude : NULL;
}

void fft_destroy(void) {
    if (!s_initialized) return;
    impl_destroy();
    shared_destroy();
    s_initialized = 0;
}
