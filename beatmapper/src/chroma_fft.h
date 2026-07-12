#pragma once
#include <math.h>

// ---------------------------------------------------------------------------
// Iterative Cooley-Tukey radix-2 in-place FFT (decimation-in-time).
// re[] and im[] must each have n elements; n must be a power of two.
// Shared header included by chroma_hps.cpp, chroma_nnls.cpp, chroma_peaks.cpp.
// ---------------------------------------------------------------------------
inline void chroma_fft(float* re, float* im, int n)
{
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    // Butterfly stages
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * 3.14159265358979f / (float)len;
        float wr  = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int j = 0; j < (len >> 1); j++) {
                float ur = re[i+j],              ui = im[i+j];
                float vr = re[i+j+len/2]*cr - im[i+j+len/2]*ci;
                float vi = re[i+j+len/2]*ci + im[i+j+len/2]*cr;
                re[i+j]       = ur + vr;  im[i+j]       = ui + vi;
                re[i+j+len/2] = ur - vr;  im[i+j+len/2] = ui - vi;
                float nr = cr*wr - ci*wi;
                float ni = cr*wi + ci*wr;
                cr = nr; ci = ni;
            }
        }
    }
}
