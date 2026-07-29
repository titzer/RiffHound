#include "chords.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

const char* const PITCH_CLASS_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

const char* const CHORD_QUALITY_SUFFIX[CQ_COUNT] = {
    "",      // CQ_NONE (unused; printed as "N")
    "5", "", "m", "7", "m7", "maj7", "sus4", "sus2", "dim"
};

// Interval sets relative to the root, in semitones.
static const int TEMPLATE_INTERVALS[CQ_COUNT][5] = {
    { -1 },                  // NONE
    { 0, 7, -1 },            // 5
    { 0, 4, 7, -1 },         // maj
    { 0, 3, 7, -1 },         // min
    { 0, 4, 7, 10, -1 },     // 7
    { 0, 3, 7, 10, -1 },     // m7
    { 0, 4, 7, 11, -1 },     // maj7
    { 0, 5, 7, -1 },         // sus4
    { 0, 2, 7, -1 },         // sus2
    { 0, 3, 6, -1 },         // dim
};

void chord_params_defaults(ChordParams* p) {
    p->self_bonus     = 0.18f;
    p->root_weight    = 1.35f;
    p->bass_weight    = 0.60f;
    p->silence_thresh = 0.06f;
    p->allow_sevenths = true;
    p->allow_sus      = true;
    p->allow_power    = true;
}

void chord_name(const ChordLabel* c, char* out, int out_size) {
    if (c->quality == CQ_NONE || c->root < 0) {
        snprintf(out, out_size, "N");
        return;
    }
    snprintf(out, out_size, "%s%s", PITCH_CLASS_NAMES[c->root],
             CHORD_QUALITY_SUFFIX[c->quality]);
}

// A candidate state in the Viterbi lattice.
struct Cand { int root; int quality; };

static int build_candidates(const ChordParams* p, Cand* out) {
    int n = 0;
    out[n].root = -1; out[n].quality = CQ_NONE; n++;
    for (int q = CQ_5; q < CQ_COUNT; q++) {
        if (!p->allow_power && q == CQ_5) continue;
        if (!p->allow_sevenths && (q == CQ_7 || q == CQ_MIN7 || q == CQ_MAJ7)) continue;
        if (!p->allow_sus && (q == CQ_SUS4 || q == CQ_SUS2)) continue;
        for (int r = 0; r < 12; r++) { out[n].root = r; out[n].quality = q; n++; }
    }
    return n;
}

// Cosine similarity between a normalised chroma vector and a chord template.
static float template_score(const float* v, float vnorm, int root, int quality,
                            const ChordParams* p) {
    float tpl[12] = {};
    const int* iv = TEMPLATE_INTERVALS[quality];
    for (int k = 0; iv[k] >= 0; k++) {
        int pc = (root + iv[k]) % 12;
        tpl[pc] = (iv[k] == 0) ? p->root_weight : 1.0f;
    }
    float dot = 0, tn = 0;
    for (int i = 0; i < 12; i++) { dot += v[i] * tpl[i]; tn += tpl[i] * tpl[i]; }
    tn = sqrtf(tn);
    if (tn <= 0 || vnorm <= 0) return 0;
    return dot / (vnorm * tn);
}

void chord_label_sequence(const float* chroma, const float* bass, int n,
                          const ChordParams* p, ChordLabel* out) {
    if (n <= 0) return;

    Cand cands[12 * CQ_COUNT + 1];
    int  ncand = build_candidates(p, cands);

    float* emit = (float*)calloc((size_t)n * ncand, sizeof(float));
    if (!emit) return;

    // ---- emission scores -------------------------------------------------
    for (int i = 0; i < n; i++) {
        float v[12];
        float energy = 0;
        for (int k = 0; k < 12; k++) {
            v[k] = chroma[i * 12 + k];
            if (bass) v[k] += p->bass_weight * bass[i * 12 + k];
            energy += v[k];
        }
        float vnorm = 0;
        for (int k = 0; k < 12; k++) vnorm += v[k] * v[k];
        vnorm = sqrtf(vnorm);

        bool silent = (energy / 12.0f) < p->silence_thresh;
        for (int c = 0; c < ncand; c++) {
            if (cands[c].quality == CQ_NONE) {
                // "no chord" wins on silence and on chroma with no clear structure
                emit[(size_t)i * ncand + c] = silent ? 0.95f : 0.34f;
            } else {
                emit[(size_t)i * ncand + c] =
                    silent ? 0.0f
                           : template_score(v, vnorm, cands[c].root, cands[c].quality, p);
            }
        }
    }

    // ---- Viterbi ---------------------------------------------------------
    float* dp   = (float*)calloc((size_t)n * ncand, sizeof(float));
    int*   back = (int*)  calloc((size_t)n * ncand, sizeof(int));
    if (!dp || !back) { free(emit); free(dp); free(back); return; }

    for (int c = 0; c < ncand; c++) dp[c] = emit[c];

    for (int i = 1; i < n; i++) {
        // Best predecessor overall; a same-state transition additionally earns
        // self_bonus, which is what suppresses one-beat flapping.
        float best = -1e30f; int bestc = 0;
        for (int c = 0; c < ncand; c++) {
            float s = dp[(size_t)(i - 1) * ncand + c];
            if (s > best) { best = s; bestc = c; }
        }
        for (int c = 0; c < ncand; c++) {
            float stay  = dp[(size_t)(i - 1) * ncand + c] + p->self_bonus;
            float score = best;
            int   prev  = bestc;
            if (stay > score) { score = stay; prev = c; }
            dp[(size_t)i * ncand + c]   = score + emit[(size_t)i * ncand + c];
            back[(size_t)i * ncand + c] = prev;
        }
    }

    // ---- traceback -------------------------------------------------------
    int cur = 0;
    {
        float best = -1e30f;
        for (int c = 0; c < ncand; c++) {
            float s = dp[(size_t)(n - 1) * ncand + c];
            if (s > best) { best = s; cur = c; }
        }
    }
    int* path = (int*)calloc(n, sizeof(int));
    if (!path) { free(emit); free(dp); free(back); return; }
    for (int i = n - 1; i >= 0; i--) {
        path[i] = cur;
        if (i > 0) cur = back[(size_t)i * ncand + cur];
    }

    for (int i = 0; i < n; i++) {
        int c = path[i];
        out[i].root    = cands[c].root;
        out[i].quality = cands[c].quality;
        out[i].score   = emit[(size_t)i * ncand + c];

        // Margin is measured against the best *other* chord at this beat, which
        // is what tells you whether the label is trustworthy.
        float second = -1e30f;
        for (int d = 0; d < ncand; d++) {
            if (d == c) continue;
            float s = emit[(size_t)i * ncand + d];
            if (s > second) second = s;
        }
        out[i].margin = out[i].score - second;
    }

    free(path); free(emit); free(dp); free(back);
}
