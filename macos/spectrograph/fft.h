/* fft.h – real-valued FFT pipeline for audio analysis
 *
 * Usage:
 *   1. Optionally adjust the global parameters below.
 *   2. Call fft_init().  Re-call fft_reinit() after changing parameters.
 *   3. Feed audio with fft_push(); it returns a magnitude pointer each time
 *      a new frame is ready, NULL otherwise.
 *   4. Call fft_destroy() when done.
 */
#pragma once

/* ── Window functions ─────────────────────────────────────────────────────── */
typedef enum {
    FFT_WINDOW_RECT     = 0,   /* rectangular (no window)  */
    FFT_WINDOW_HANN     = 1,   /* Hann           (default) */
    FFT_WINDOW_HAMMING  = 2,   /* Hamming                  */
    FFT_WINDOW_BLACKMAN = 3,   /* Blackman                 */
} FftWindowType;

/* ── Global parameters ────────────────────────────────────────────────────── *
 * Change any of these and call fft_reinit() to apply.
 * They are extern so that UI code can point directly at them.              */
extern int           fft_size;          /* frame length in samples (power of 2) */
extern int           fft_hop_size;      /* samples between successive frames    */
extern FftWindowType fft_window_type;   /* window applied before each FFT       */
extern double        fft_sample_rate;   /* audio sample rate in Hz              */

/* ── Read-only derived values (written by fft_init / fft_reinit) ─────────── */
extern int    fft_bin_count;        /* output bins = fft_size / 2 + 1           */
extern double fft_freq_resolution;  /* Hz per bin  = fft_sample_rate / fft_size */

/* ── Lifecycle ────────────────────────────────────────────────────────────── */
int  fft_init(void);     /* allocate; returns 0 on success, -1 on error */
int  fft_reinit(void);   /* tear down and rebuild after parameter changes */
void fft_destroy(void);  /* free all resources                           */

/* ── Processing ──────────────────────────────────────────────────────────── *
 * Push mono float32 audio samples into the pipeline.  When a full hop has
 * accumulated the FFT is computed automatically.
 *
 * Returns a pointer to fft_bin_count linear-scale magnitude values when a
 * new frame is ready, NULL otherwise.  The pointer is valid until the next
 * call to fft_push() or fft_destroy().                                     */
const float *fft_push(const float *samples, int count);

/* The most recent magnitude output (NULL before the first frame). */
const float *fft_output(void);
