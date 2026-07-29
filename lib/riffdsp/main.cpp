// riffdsp — command-line front end to the RiffHound analysis routines.
//
// The DSP itself is the code the beatmapper GUI already uses (beat_algo,
// beat_spectral_flux, chroma_*).  Those files are pure C++ with no UI
// dependency, so this tool compiles them directly out of beatmapper/src.
// One implementation, two front ends: the editor for humans, this for scripts
// and agents.

#include "timeseries.h"
#include "audio_io.h"
#include "chords.h"
#include "beat_algo.h"
#include "chroma_algo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void usage() {
    fprintf(stderr,
        "usage: riffdsp <command> [options] <audio-file>\n"
        "\n"
        "commands:\n"
        "  beats    detect beats and print a beatmap in timeseries format\n"
        "  onsets   print raw onset times\n"
        "  grid     fit a global tempo/phase grid to the onsets (best for\n"
        "           unattended whole-track beatmapping)\n"
        "  chroma   print one beat-synchronous 12-bin chroma vector per beat\n"
        "  chords   label each beat with a chord and print chord: events\n"
        "  info     print duration and sample rate\n"
        "\n"
        "options:\n"
        "  --beats FILE      read beat times from a timeseries file\n"
        "                    (required by chroma/chords)\n"
        "  --start SEC       analysis window start (default 0)\n"
        "  --end SEC         analysis window end (default end of track)\n"
        "  --min-bpm N       tempo search floor (default 60)\n"
        "  --max-bpm N       tempo search ceiling (default 200)\n"
        "  --algo NAME       chroma algorithm: nnls|goertzel|hps|peaks\n"
        "  --self-bonus F    chord continuity strength (default 0.18)\n"
        "  --no-sevenths     restrict chord vocabulary to triads and power chords\n"
        "  --no-sus          drop sus2/sus4 from the vocabulary\n"
        "  --hint-bpm N      known tempo; restricts the grid search around it\n"
        "  --support-exp F   grid-line support weight (0=off, default 0.5)\n"
        "  --min-margin F    label beats below this margin as uncertain\n"
        "  --tsv             chords: print a per-beat table instead of events\n");
}

struct Opts {
    const char* beats_file  = nullptr;
    double      start       = 0.0;
    double      end         = 0.0;   // 0 = to end of track
    float       min_bpm     = 60.0f;
    float       max_bpm     = 200.0f;
    const char* algo        = "nnls";
    float       self_bonus  = 0.18f;
    bool        no_sevenths = false;
    bool        no_sus      = false;
    float       min_margin  = 0.0f;
    bool        tsv         = false;
    float       hint_bpm    = 0.0f;   // 0 = no hint
    float       hint_tol    = 0.06f;  // +/- fraction around the hint
};

static ChromaFn pick_chroma(const char* name) {
    for (int i = 0; i < CHROMA_ALGO_COUNT; i++) {
        // match on a case-insensitive prefix of the registered name
        const char* n = CHROMA_ALGOS[i].name;
        size_t len = strlen(name);
        bool hit = true;
        for (size_t k = 0; k < len; k++) {
            char a = n[k], b = name[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { hit = false; break; }
        }
        if (hit) return CHROMA_ALGOS[i].fn;
    }
    return nullptr;
}

// Read beat times from a timeseries file.  Returns count, or -1 on error.
static int load_beats(const char* path, double** out) {
    TsFile ts;
    ts_init(&ts);
    if (!ts_load(&ts, path)) {
        fprintf(stderr, "riffdsp: cannot read beats file '%s'\n", path);
        ts_shutdown(&ts);
        return -1;
    }
    double* buf = (double*)malloc(sizeof(double) * (ts.count + 1));
    if (!buf) { ts_shutdown(&ts); return -1; }
    int n = ts_beat_times(&ts, buf, ts.count + 1);
    ts_shutdown(&ts);
    *out = buf;
    return n;
}

// Beat-synchronous chroma: one vector per beat interval [b[i], b[i+1]).
// Also fills a bass-band chroma when out_bass is non-null; the low band is a
// strong root cue and is what disambiguates a G chord from its Em relative.
static void beat_chroma(const float* pcm, uint64_t frames, uint32_t ch,
                        uint32_t sr, const double* beats, int nbeats,
                        ChromaFn fn, float* out, float* out_bass,
                        const float* lowpcm) {
    for (int i = 0; i + 1 < nbeats; i++) {
        fn(pcm, frames, ch, sr, beats[i], beats[i + 1], out + i * 12);
        if (out_bass && lowpcm)
            fn(lowpcm, frames, ch, sr, beats[i], beats[i + 1], out_bass + i * 12);
    }
}

// One-pole low-pass at ~220 Hz, applied to a copy of the buffer, to isolate
// the bass register for root detection.
static float* make_lowband(const float* pcm, uint64_t frames, uint32_t ch,
                           uint32_t sr) {
    size_t n = (size_t)frames * ch;
    float* lp = (float*)malloc(n * sizeof(float));
    if (!lp) return nullptr;
    const float fc    = 220.0f;
    const float alpha = 1.0f - expf(-2.0f * 3.14159265f * fc / (float)sr);
    for (uint32_t c = 0; c < ch; c++) {
        float y = 0;
        for (uint64_t i = 0; i < frames; i++) {
            float x = pcm[i * ch + c];
            y += alpha * (x - y);
            lp[i * ch + c] = y;
        }
    }
    return lp;
}

// Collect raw onsets across a whole track by running the detector in chunks.
// Returns the count; *out is malloc'd and owned by the caller.
static int collect_onsets(const float* pcm, uint64_t frames, uint32_t ch,
                          uint32_t sr, double t0, double t1,
                          const BeatAlgoParams* bp, double** out,
                          float* out_bpm) {
    int    cap = 4096, n = 0;
    double* buf = (double*)malloc(cap * sizeof(double));
    if (!buf) return -1;

    AutoBeatList ab;
    autobeat_init(&ab);
    double bpm_sum = 0; int bpm_n = 0;

    const double CHUNK = 45.0;
    for (double t = t0; t < t1 - 0.05; ) {
        double t2 = t + CHUNK; if (t2 > t1) t2 = t1;
        beat_spectral_flux(pcm, frames, ch, sr, t, t2, bp, &ab);
        if (ab.estimated_bpm > 0) { bpm_sum += ab.estimated_bpm; bpm_n++; }
        for (int i = 0; i < ab.onset_count; i++) {
            double ot = ab.onset_times[i];
            if (ot < t || ot >= t2) continue;
            if (n > 0 && ot <= buf[n - 1] + 0.005) continue;   // de-dup
            if (n == cap) {
                cap *= 2;
                double* nb = (double*)realloc(buf, cap * sizeof(double));
                if (!nb) { free(buf); return -1; }
                buf = nb;
            }
            buf[n++] = ot;
        }
        if (t2 >= t1) break;
        t = t2;
    }
    *out = buf;
    if (out_bpm) *out_bpm = bpm_n ? (float)(bpm_sum / bpm_n) : 0.0f;
    return n;
}

// How well a grid of period T starting at phase p explains the onsets.
//
// Two terms, multiplied:
//   (a) onset agreement -- how close onsets sit to grid lines
//   (b) line support    -- what fraction of grid lines have any onset near them
//
// (b) is what prevents the octave error.  Doubling the tempo always explains at
// least as many onsets as the true tempo (every real beat still lands on a line,
// and the eighth-note offbeats now land on lines too), so (a) alone is biased
// towards fast grids.  But at double tempo roughly half the grid lines have no
// onset at all, which (b) penalises directly.
static double g_support_exp = 0.5;   // tuned against riffeval; see findings

static double grid_score(const double* on, int n, double t0, double t1,
                         double T, double phase) {
    if (T <= 0 || t1 <= t0) return 0;
    double tol = 0.12 * T;
    int nlines = (int)((t1 - phase) / T) + 1;
    if (nlines < 4) return 0;

    // Mark which grid lines got support, and accumulate onset agreement.
    static const int MAXLINES = 65536;
    if (nlines > MAXLINES) nlines = MAXLINES;
    static bool supported[MAXLINES];
    memset(supported, 0, sizeof(bool) * nlines);

    double agree = 0;
    int used = 0;
    for (int i = 0; i < n; i++) {
        if (on[i] < t0 || on[i] > t1) continue;
        used++;
        double k = (on[i] - phase) / T;
        long   ki = (long)floor(k + 0.5);
        double d  = fabs(k - ki) * T;
        if (d < tol) {
            agree += 1.0 - d / tol;
            if (ki >= 0 && ki < nlines) supported[ki] = true;
        }
    }
    if (!used) return 0;

    int nsup = 0;
    for (int i = 0; i < nlines; i++) if (supported[i]) nsup++;
    double support_frac = (double)nsup / nlines;

    return (agree / used) * pow(support_frac, g_support_exp);
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }

    const char* cmd  = argv[1];
    const char* path = nullptr;
    Opts o;

    for (int i = 2; i < argc; i++) {
        const char* a = argv[i];
        if      (!strcmp(a, "--beats")      && i + 1 < argc) o.beats_file = argv[++i];
        else if (!strcmp(a, "--start")      && i + 1 < argc) o.start      = atof(argv[++i]);
        else if (!strcmp(a, "--end")        && i + 1 < argc) o.end        = atof(argv[++i]);
        else if (!strcmp(a, "--min-bpm")    && i + 1 < argc) o.min_bpm    = (float)atof(argv[++i]);
        else if (!strcmp(a, "--max-bpm")    && i + 1 < argc) o.max_bpm    = (float)atof(argv[++i]);
        else if (!strcmp(a, "--algo")       && i + 1 < argc) o.algo       = argv[++i];
        else if (!strcmp(a, "--self-bonus") && i + 1 < argc) o.self_bonus = (float)atof(argv[++i]);
        else if (!strcmp(a, "--min-margin") && i + 1 < argc) o.min_margin = (float)atof(argv[++i]);
        else if (!strcmp(a, "--no-sevenths")) o.no_sevenths = true;
        else if (!strcmp(a, "--no-sus"))      o.no_sus      = true;
        else if (!strcmp(a, "--tsv"))         o.tsv         = true;
        else if (!strcmp(a, "--support-exp") && i + 1 < argc) g_support_exp = atof(argv[++i]);
        else if (!strcmp(a, "--hint-bpm")  && i + 1 < argc) o.hint_bpm = (float)atof(argv[++i]);
        else if (!strcmp(a, "--hint-tol")  && i + 1 < argc) o.hint_tol = (float)atof(argv[++i]);
        else if (a[0] == '-') { fprintf(stderr, "riffdsp: unknown option %s\n", a); return 1; }
        else path = a;
    }
    if (!path) { usage(); return 1; }

    float*   pcm    = nullptr;
    uint64_t frames = 0;
    uint32_t ch = 0, sr = 0;
    if (!audio_decode(path, &pcm, &frames, &ch, &sr)) return 1;
    double duration = (double)frames / sr;
    if (o.end <= 0 || o.end > duration) o.end = duration;

    if (!strcmp(cmd, "info")) {
        printf("file\t%s\n", path);
        printf("duration\t%.3f\n", duration);
        printf("sample_rate\t%u\n", sr);
        printf("channels\t%u\n", ch);
        free(pcm);
        return 0;
    }

    if (!strcmp(cmd, "beats")) {
        BeatAlgoParams bp = {};
        bp.min_bpm         = o.min_bpm;
        bp.max_bpm         = o.max_bpm;
        bp.onset_threshold = 1.5f;
        bp.dp_tightness    = 400.0f;
        bp.pre_onset_ms    = 30.0f;
        bp.seed_times      = nullptr;
        bp.seed_count      = 0;

        AutoBeatList ab;
        autobeat_init(&ab);

        // The detector output is capped at MAX_BEAT_CANDS, so a long track is
        // analysed in overlapping chunks and the results stitched together.
        const double CHUNK = 60.0;   // seconds
        const double OVER  =  4.0;   // overlap, discarded from the tail
        printf("# Beatmap  ~src=riffdsp/spectral_flux\n");
        double t = o.start;
        double last_emitted = -1e9;
        double bpm_sum = 0; int bpm_n = 0;
        while (t < o.end - 0.1) {
            double t2 = t + CHUNK; if (t2 > o.end) t2 = o.end;
            beat_spectral_flux(pcm, frames, ch, sr, t, t2, &bp, &ab);
            if (ab.estimated_bpm > 0) { bpm_sum += ab.estimated_bpm; bpm_n++; }
            double cutoff = (t2 >= o.end) ? t2 : t2 - OVER;
            for (int i = 0; i < ab.beat_count; i++) {
                double bt = ab.beat_times[i];
                if (bt < t || bt >= cutoff) continue;
                if (bt - last_emitted < 0.15) continue;   // de-dup across chunks
                printf("%.6f\t%.6f\tB\n", bt, bt);
                last_emitted = bt;
            }
            if (t2 >= o.end) break;
            t = cutoff;
        }
        if (bpm_n) fprintf(stderr, "riffdsp: mean estimated tempo %.1f BPM\n", bpm_sum / bpm_n);
        free(pcm);
        return 0;
    }

    if (!strcmp(cmd, "onsets") || !strcmp(cmd, "grid")) {
        BeatAlgoParams bp = {};
        bp.min_bpm         = o.min_bpm;
        bp.max_bpm         = o.max_bpm;
        bp.onset_threshold = 1.5f;
        bp.dp_tightness    = 400.0f;
        bp.pre_onset_ms    = 30.0f;

        double* on = nullptr;
        float est_bpm = 0;
        int non = collect_onsets(pcm, frames, ch, sr, o.start, o.end, &bp, &on, &est_bpm);
        if (non <= 0) { fprintf(stderr, "riffdsp: no onsets found\n"); free(pcm); return 1; }

        if (!strcmp(cmd, "onsets")) {
            printf("# Onsets  ~src=riffdsp/spectral_flux\n");
            for (int i = 0; i < non; i++) printf("%.6f\t%.6f\tonset\n", on[i], on[i]);
            free(on); free(pcm);
            return 0;
        }

        // --- global tempo/phase search -----------------------------------
        // The per-chunk DP tracker inserts spurious beats on syncopated
        // material, but the *tempo estimate* is reliable.  Fitting one global
        // grid to the onsets and then letting it drift locally is far more
        // robust for unattended whole-track work.
        double bestT = 0, bestP = 0, bestS = -1;
        double Tlo = 60.0 / o.max_bpm, Thi = 60.0 / o.min_bpm;
        // An external tempo (from a tab, an ID3 tag, or a sibling track) is the
        // cheapest way to settle the half/double-tempo ambiguity, which is the
        // single largest source of error in unattended beatmapping.
        if (o.hint_bpm > 0) {
            Tlo = 60.0 / (o.hint_bpm * (1.0 + o.hint_tol));
            Thi = 60.0 / (o.hint_bpm * (1.0 - o.hint_tol));
            fprintf(stderr, "riffdsp: tempo hint %.1f BPM, searching %.1f-%.1f BPM\n",
                    o.hint_bpm, 60.0 / Thi, 60.0 / Tlo);
        }
        for (double T = Tlo; T <= Thi; T += 0.0005) {
            // Coarse phase sweep; the kernel is smooth so this is enough.
            for (double ph = 0; ph < T; ph += T / 100.0) {
                double s = grid_score(on, non, o.start, o.end, T, o.start + ph);
                if (s > bestS) { bestS = s; bestT = T; bestP = o.start + ph; }
            }
        }
        fprintf(stderr, "riffdsp: grid fit %.2f BPM (period %.4f s), support %.1f/%d onsets\n",
                60.0 / bestT, bestT, bestS, non);
        if (est_bpm > 0)
            fprintf(stderr, "riffdsp: chunked tempo estimate was %.1f BPM\n", est_bpm);

        // --- anchor to onsets, then local linear refit ---------------------
        int nb = (int)((o.end - bestP) / bestT) + 1;
        if (nb < 2) { fprintf(stderr, "riffdsp: grid too short\n"); free(on); free(pcm); return 1; }
        double* anchor = (double*)malloc(nb * sizeof(double));
        bool*   has    = (bool*)  calloc(nb, sizeof(bool));
        for (int k = 0; k < nb; k++) {
            double g = bestP + k * bestT;
            double best = 1e30; int bi = -1;
            for (int i = 0; i < non; i++) {
                double d = fabs(on[i] - g);
                if (d < best) { best = d; bi = i; }
            }
            if (bi >= 0 && best < 0.25 * bestT) { anchor[k] = on[bi]; has[k] = true; }
        }

        // Local linear regression of anchor time on beat index, which tracks
        // gradual tempo drift without chasing individual mis-detections.
        const int W = 16;
        printf("# Beatmap  ~src=riffdsp/grid ~bpm=%.2f\n", 60.0 / bestT);
        double prev = -1e9;
        int emitted = 0, anchored = 0;
        for (int k = 0; k < nb; k++) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
            for (int j = k - W; j <= k + W; j++) {
                if (j < 0 || j >= nb || !has[j]) continue;
                sx += j; sy += anchor[j]; sxx += (double)j * j; sxy += (double)j * anchor[j];
                m++;
            }
            double t;
            if (m >= 4) {
                double den = m * sxx - sx * sx;
                if (fabs(den) < 1e-9) t = bestP + k * bestT;
                else {
                    double slope = (m * sxy - sx * sy) / den;
                    double icpt  = (sy - slope * sx) / m;
                    t = icpt + slope * k;
                }
            } else {
                t = bestP + k * bestT;
            }
            if (has[k]) anchored++;
            if (t < o.start - 0.01 || t > o.end) continue;
            if (t <= prev + 0.05) continue;
            printf("%.6f\t%.6f\tB\n", t, t);
            prev = t;
            emitted++;
        }
        fprintf(stderr, "riffdsp: emitted %d beats, %d/%d had onset support (%.0f%%)\n",
                emitted, anchored, nb, 100.0 * anchored / nb);
        free(anchor); free(has); free(on); free(pcm);
        return 0;
    }

    // chroma and chords both need a beatmap.
    if (!o.beats_file) {
        fprintf(stderr, "riffdsp: %s requires --beats FILE\n", cmd);
        free(pcm);
        return 1;
    }
    double* beats = nullptr;
    int nbeats = load_beats(o.beats_file, &beats);
    if (nbeats < 2) {
        fprintf(stderr, "riffdsp: need at least 2 beats, got %d\n", nbeats);
        free(pcm); free(beats);
        return 1;
    }

    ChromaFn fn = pick_chroma(o.algo);
    if (!fn) {
        fprintf(stderr, "riffdsp: unknown chroma algorithm '%s'; available:\n", o.algo);
        for (int i = 0; i < CHROMA_ALGO_COUNT; i++)
            fprintf(stderr, "  %s — %s\n", CHROMA_ALGOS[i].name, CHROMA_ALGOS[i].tip);
        free(pcm); free(beats);
        return 1;
    }

    int nint = nbeats - 1;    // number of beat intervals
    float* chroma = (float*)calloc((size_t)nint * 12, sizeof(float));
    float* bass   = (float*)calloc((size_t)nint * 12, sizeof(float));
    float* lowpcm = make_lowband(pcm, frames, ch, sr);
    beat_chroma(pcm, frames, ch, sr, beats, nbeats, fn, chroma, bass, lowpcm);

    if (!strcmp(cmd, "chroma")) {
        printf("# beat\tt_start\tt_end");
        for (int k = 0; k < 12; k++) printf("\t%s", PITCH_CLASS_NAMES[k]);
        printf("\n");
        for (int i = 0; i < nint; i++) {
            printf("%d\t%.6f\t%.6f", i + 1, beats[i], beats[i + 1]);
            for (int k = 0; k < 12; k++) printf("\t%.4f", chroma[i * 12 + k]);
            printf("\n");
        }
    } else if (!strcmp(cmd, "chords")) {
        ChordParams cp;
        chord_params_defaults(&cp);
        cp.self_bonus     = o.self_bonus;
        cp.allow_sevenths = !o.no_sevenths;
        cp.allow_sus      = !o.no_sus;

        ChordLabel* labels = (ChordLabel*)calloc(nint, sizeof(ChordLabel));
        chord_label_sequence(chroma, bass, nint, &cp, labels);

        if (o.tsv) {
            printf("# beat\tt_start\tt_end\tchord\tscore\tmargin\n");
            for (int i = 0; i < nint; i++) {
                char nm[16];
                chord_name(&labels[i], nm, sizeof(nm));
                printf("%d\t%.6f\t%.6f\t%s\t%.3f\t%.3f\n",
                       i + 1, beats[i], beats[i + 1], nm,
                       labels[i].score, labels[i].margin);
            }
        } else {
            // Collapse runs of identical labels into one chord: event.
            printf("# Chords  ~src=riffdsp/chroma-%s\n", o.algo);
            int i = 0;
            while (i < nint) {
                int j = i;
                float minmargin = labels[i].margin;
                while (j + 1 < nint &&
                       labels[j + 1].root == labels[i].root &&
                       labels[j + 1].quality == labels[i].quality) {
                    j++;
                    if (labels[j].margin < minmargin) minmargin = labels[j].margin;
                }
                char nm[16];
                chord_name(&labels[i], nm, sizeof(nm));
                if (strcmp(nm, "N") != 0) {
                    const char* flag = (minmargin < o.min_margin) ? " ~uncertain" : "";
                    printf("%.6f\t%.6f\tchord: %s\t# ~beats=B%d-B%d ~margin=%.3f%s\n",
                           beats[i], beats[j + 1], nm, i + 1, j + 1, minmargin, flag);
                }
                i = j + 1;
            }
        }
        free(labels);
    } else {
        fprintf(stderr, "riffdsp: unknown command '%s'\n", cmd);
        usage();
    }

    free(chroma); free(bass); free(lowpcm); free(beats); free(pcm);
    return 0;
}
