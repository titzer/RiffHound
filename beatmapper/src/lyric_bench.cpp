// lyrbench: score the lyric aligner against hand-placed lyrics.
//
//   lyrbench [--keep K | --step | --sparse N] [--seq] [--set name=value ...]
//            MAP VOCALS [MAP VOCALS ...]
//
// For each track, the first K placed lyrics stay placed (anchors), the rest
// are unplaced and aligned to the vocal stem after the last anchor.  Reports
// how many land with their start within 0.3 s / 1 s of the annotation.
// --step scores the interactive use instead: for every k, the first k lines
// are placed as annotated and the rest aligned; counts how often the next
// line (and the three next) start within 0.5 s.
// --sparse N places every Nth line (from N/2) and aligns the rest around
// them: lyrics placed by hand in the middle.  --seq tells the aligner where
// each placed line sits among the others (as the app infers from the paste
// order); without it the alignment decides.
// scripts/lyric-bench.sh finds the maps and cached vocal stems.

#include "lyric_align.h"
#include "audio.h"
#include "beatmap.h"
#include "sectionmap.h"
#include "lyricmap.h"
#include "miscmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <string>

static bool set_param(LyricAlignParams* p, const char* kv) {
    const char* eq = strchr(kv, '=');
    if (!eq) return false;
    std::string k(kv, eq - kv);
    double v = atof(eq + 1);
#define F(name) if (k == #name) { p->name = (decltype(p->name))v; return true; }
    F(on_frac) F(max_below) F(bridge_gap) F(min_unit) F(split_len) F(split_dip)
    F(rate_lo) F(rate_hi) F(max_run) F(w_dur) F(w_nuc) F(w_inner) F(inner_ok)
    F(w_break) F(break_ok) F(w_skip) F(c_skip) F(c_unplaced) F(horizon)
#undef F
    return false;
}

int main(int argc, char** argv) {
    LyricAlignParams p;
    lyric_align_defaults(&p);
    int keep = 0;
    bool verbose = false, step = false, seq = false;
    int sparse = 0;
    int st_n = 0, st_next = 0, st_next3 = 0;
    int ai = 1;
    while (ai < argc && argv[ai][0] == '-') {
        if (!strcmp(argv[ai], "--keep") && ai + 1 < argc) { keep = atoi(argv[ai + 1]); ai += 2; }
        else if (!strcmp(argv[ai], "--set") && ai + 1 < argc) {
            if (!set_param(&p, argv[ai + 1])) { fprintf(stderr, "unknown param %s\n", argv[ai + 1]); return 2; }
            ai += 2;
        }
        else if (!strcmp(argv[ai], "-v")) { verbose = true; ai++; }
        else if (!strcmp(argv[ai], "--step")) { step = true; ai++; }
        else if (!strcmp(argv[ai], "--seq")) { seq = true; ai++; }
        else if (!strcmp(argv[ai], "--sparse") && ai + 1 < argc) { sparse = atoi(argv[ai + 1]); ai += 2; }
        else { fprintf(stderr, "unknown flag %s\n", argv[ai]); return 2; }
    }
    int tot = 0, tot03 = 0, tot1 = 0, tot_placed = 0;
    double score_hit = 0, score_miss = 0; int n_hit = 0, n_miss = 0;
    for (; ai + 1 < argc; ai += 2) {
        const char* map = argv[ai];
        const char* voc = argv[ai + 1];
        BeatMap bm; SectionMap sm; LyricMap lm; MiscMap mm, cm;
        beatmap_init(&bm); sectionmap_init(&sm); lyricmap_init(&lm);
        miscmap_init(&mm); miscmap_init(&cm, "chord");
        if (!beatmap_load(&bm, &sm, &lm, &mm, &cm, map)) { fprintf(stderr, "cannot load %s\n", map); continue; }
        float* pcm = nullptr; uint64_t nf = 0; uint32_t sr = 0;
        if (!audio_decode_pcm(voc, &pcm, &nf, &sr)) { fprintf(stderr, "cannot decode %s\n", voc); continue; }
        double dur = (double)nf / sr;
        LyricActivity act;
        lyric_activity(pcm, nf, sr, &act);
        audio_free_pcm(pcm);

        if (step) {
            std::vector<const Lyric*> all;
            for (int i = 0; i < lm.count; i++)
                if (lm.lyrics[i].t_start < dur) all.push_back(&lm.lyrics[i]);
            int n_next = 0, n_next3 = 0, n = 0;
            for (size_t k = 0; k < all.size(); k++) {
                std::vector<LyricAnchor> anc;
                for (size_t a = 0; a < k; a++) anc.push_back({ all[a]->t_start, all[a]->t_end, all[a]->text, seq ? 0 : -1 });
                std::vector<const char*> rest;
                for (size_t a = k; a < all.size(); a++) rest.push_back(all[a]->text);
                std::vector<LyricAlignOut> out;
                lyric_align(act, 0.0, dur, rest, anc, p, &out);
                bool ok = true; int good = 0;
                for (size_t a = 0; a < 3 && a < rest.size(); a++) {
                    bool hit = out[a].placed && fabs(out[a].t0 - all[k + a]->t_start) <= 0.5;
                    if (a == 0 && hit) n_next++;
                    if (hit && ok) good++; else ok = false;
                }
                if (good == 3 || good == (int)rest.size()) n_next3++;
                n++;
                if (verbose)
                    printf("  %s %7.2f %7.2f  score %.2f  %s\n", out[0].placed && fabs(out[0].t0 - all[k]->t_start) <= 0.5 ? " " : "*",
                           all[k]->t_start, out[0].t0, out[0].score, all[k]->text);
            }
            const char* name = strrchr(map, '/'); name = name ? name + 1 : map;
            printf("%-34.34s steps %3d  next %3d (%5.1f%%)  next3 %3d (%5.1f%%)\n", name, n,
                   n_next, n ? 100.0 * n_next / n : 0.0, n_next3, n ? 100.0 * n_next3 / n : 0.0);
            st_n += n; st_next += n_next; st_next3 += n_next3;
            beatmap_shutdown(&bm); sectionmap_shutdown(&sm); lyricmap_shutdown(&lm);
            miscmap_shutdown(&mm); miscmap_shutdown(&cm);
            continue;
        }
        std::vector<LyricAnchor> anchors;
        std::vector<const char*> lines;
        std::vector<double> truth;
        double t0 = 0.0;
        int k_in = 0;
        for (int i = 0; i < lm.count; i++) {
            const Lyric& ly = lm.lyrics[i];
            if (ly.t_start >= dur) continue;
            int k = k_in++;
            if (sparse > 0 && k % sparse == sparse / 2) {
                anchors.push_back({ ly.t_start, ly.t_end, ly.text, seq ? (int)lines.size() : -1 });
            } else if (sparse == 0 && (int)anchors.size() < keep) {
                anchors.push_back({ ly.t_start, ly.t_end, ly.text, seq ? 0 : -1 });
                t0 = ly.t_end;
            } else {
                lines.push_back(ly.text);
                truth.push_back(ly.t_start);
            }
        }
        std::vector<LyricAlignOut> out;
        (void)t0;
        double rate = lyric_align(act, 0.0, dur, lines, anchors, p, &out);
        int n03 = 0, n1 = 0, placed = 0;
        std::vector<double> errs;
        for (size_t i = 0; i < lines.size(); i++) {
            if (!out[i].placed) continue;
            placed++;
            double e = fabs(out[i].t0 - truth[i]);
            errs.push_back(e);
            if (e <= 0.3) n03++;
            if (e <= 1.0) n1++;
            if (e <= 1.0) { score_hit += out[i].score; n_hit++; }
            else          { score_miss += out[i].score; n_miss++; }
            if (verbose)
                printf("  %s %7.2f %7.2f  err %6.2f  score %.2f  %s\n", e <= 1.0 ? " " : "*",
                       truth[i], out[i].t0, out[i].t0 - truth[i], out[i].score, lines[i]);
        }
        std::sort(errs.begin(), errs.end());
        const char* name = strrchr(map, '/'); name = name ? name + 1 : map;
        printf("%-34.34s lines %3d placed %3d  <=0.3s %3d  <=1s %3d  median %5.2fs  rate %.3f\n",
               name, (int)lines.size(), placed, n03, n1,
               errs.empty() ? 0.0 : errs[errs.size() / 2], rate);
        tot += (int)lines.size(); tot03 += n03; tot1 += n1; tot_placed += placed;
        beatmap_shutdown(&bm); sectionmap_shutdown(&sm); lyricmap_shutdown(&lm);
        miscmap_shutdown(&mm); miscmap_shutdown(&cm);
    }
    if (step) {
        printf("STEP total %d  next %d (%.1f%%)  next3 %d (%.1f%%)\n", st_n, st_next,
               st_n ? 100.0 * st_next / st_n : 0.0, st_next3, st_n ? 100.0 * st_next3 / st_n : 0.0);
        return 0;
    }
    printf("TOTAL lines %d placed %d  <=0.3s %d (%.1f%%)  <=1s %d (%.1f%%)  score hit %.2f miss %.2f\n",
           tot, tot_placed, tot03, tot ? 100.0 * tot03 / tot : 0.0, tot1, tot ? 100.0 * tot1 / tot : 0.0,
           n_hit ? score_hit / n_hit : 0.0, n_miss ? score_miss / n_miss : 0.0);
    return 0;
}
