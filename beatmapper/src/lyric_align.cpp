#include "lyric_align.h"
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <algorithm>
#include <string>

void lyric_align_defaults(LyricAlignParams* p) { *p = LyricAlignParams(); }

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

void lyric_activity(const float* mono, uint64_t frames, uint32_t sr, LyricActivity* out)
{
    out->db.clear();
    out->floor_db = out->peak_db = -90.0f;
    if (!mono || frames == 0 || sr == 0) return;
    const int hop = (int)(sr * out->hop + 0.5);
    const uint64_t n_fr = frames / (uint64_t)hop;

    // One-pole high-pass at ~120 Hz drops rumble and stem bleed from the bass.
    const float a = expf(-2.0f * (float)M_PI * 120.0f / (float)sr);
    float prev_x = 0.0f, prev_y = 0.0f;
    // Energy per 10 ms hop, then averaged over a 30 ms window.
    std::vector<float> hop_e((size_t)n_fr, 0.0f);
    for (uint64_t f = 0; f < n_fr; f++) {
        double acc = 0.0;
        const float* x = mono + f * (uint64_t)hop;
        for (int k = 0; k < hop; k++) {
            float y = a * (prev_y + x[k] - prev_x);
            prev_x = x[k]; prev_y = y;
            acc += (double)y * y;
        }
        hop_e[f] = (float)(acc / hop);
    }
    out->db.resize((size_t)n_fr);
    for (uint64_t f = 0; f < n_fr; f++) {
        float s = hop_e[f], c = 1.0f;
        if (f > 0)        { s += hop_e[f - 1]; c += 1.0f; }
        if (f + 1 < n_fr) { s += hop_e[f + 1]; c += 1.0f; }
        out->db[f] = 10.0f * log10f(s / c + 1e-10f);
    }

    std::vector<float> sorted;
    sorted.reserve(out->db.size());
    for (float d : out->db) if (d > -95.0f) sorted.push_back(d);
    if (sorted.empty()) return;
    std::sort(sorted.begin(), sorted.end());
    out->floor_db = sorted[(size_t)(0.15 * (sorted.size() - 1))];
    out->peak_db  = sorted[(size_t)(0.98 * (sorted.size() - 1))];
}

// ---------------------------------------------------------------------------
// Syllables
// ---------------------------------------------------------------------------

static bool is_vowel(char c) { return strchr("aeiouy", c) != nullptr; }

static int word_syllables(const char* w, int len) {
    char b[64];
    int n = 0;
    for (int i = 0; i < len && n < 63; i++)
        if (isalpha((unsigned char)w[i])) b[n++] = (char)tolower((unsigned char)w[i]);
    b[n] = 0;
    if (n == 0) {
        for (int i = 0; i < len; i++) if (isdigit((unsigned char)w[i])) return 2;  // "99"
        return 0;
    }
    int groups = 0;
    for (int i = 0; i < n; i++)
        if (is_vowel(b[i]) && (i == 0 || !is_vowel(b[i - 1]))) groups++;
    // Silent final e ("make", "love"), but not "-le" ("little") or "the".
    if (n > 2 && b[n - 1] == 'e' && !is_vowel(b[n - 2]) &&
        !(b[n - 2] == 'l' && !is_vowel(b[n - 3])) && groups > 1)
        groups--;
    // "-ed" is silent unless after t/d ("walked" vs "wanted").
    if (n > 3 && b[n - 2] == 'e' && b[n - 1] == 'd' && b[n - 3] != 't' && b[n - 3] != 'd' &&
        !is_vowel(b[n - 3]) && groups > 1)
        groups--;
    return groups < 1 ? 1 : groups;
}

int lyric_syllables(const char* text) {
    int total = 0;
    const char* p = text;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == '-' || *p == '/')) p++;
        const char* w = p;
        while (*p && !isspace((unsigned char)*p) && *p != '-' && *p != '/') p++;
        if (p > w) total += word_syllables(w, (int)(p - w));
    }
    return total;
}

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------

static float max_in(const std::vector<float>& v, int a, int b) {
    float m = -200.0f;
    for (int i = a; i < b; i++) if (v[i] > m) m = v[i];
    return m;
}

// Split frames [a, b) at deep dips while longer than split_len.
static void split_run(const LyricActivity& act, int a, int b, const LyricAlignParams& p,
                      std::vector<std::pair<int, float>>* cuts)
{
    const double hop = act.hop;
    if ((b - a) * hop <= p.split_len) return;
    const int edge = (int)(0.25 / hop), side = (int)(0.4 / hop);
    int best = -1; float best_d = p.split_dip;
    for (int i = a + edge; i < b - edge; i++) {
        float d = act.db[i];
        if (d > act.db[i - 1] || d > act.db[i + 1]) continue;
        int l0 = std::max(a, i - side), r1 = std::min(b, i + side + 1);
        float depth = std::min(max_in(act.db, l0, i), max_in(act.db, i + 1, r1)) - d;
        if (depth > best_d) { best_d = depth; best = i; }
    }
    if (best < 0) return;
    split_run(act, a, best, p, cuts);
    cuts->push_back({ best, best_d });
    split_run(act, best, b, p, cuts);
}

static int count_nuclei(const LyricActivity& act, int a, int b) {
    const int w = 8, reach = 15;   // a peak tops +-80 ms and rises 3 dB within 150 ms
    int n = 0;
    for (int i = a; i < b; i++) {
        float d = act.db[i];
        int l = std::max(a, i - w), r = std::min(b, i + w + 1);
        if (max_in(act.db, l, r) > d) continue;
        if (i > a && act.db[i - 1] == d) continue;     // plateau: count once
        float lmin = d, rmin = d;
        for (int k = std::max(a, i - reach); k < i; k++) lmin = std::min(lmin, act.db[k]);
        for (int k = i + 1; k < std::min(b, i + reach + 1); k++) rmin = std::min(rmin, act.db[k]);
        if (i - reach < a) lmin = std::min(lmin, d - 6.0f);   // unit edges count as low
        if (i + reach >= b) rmin = std::min(rmin, d - 6.0f);
        if (d - lmin >= 3.0f && d - rmin >= 3.0f) n++;
    }
    return n < 1 ? 1 : n;
}

void lyric_units(const LyricActivity& act, double t0, double t1,
                 const LyricAlignParams& p, std::vector<LyricUnit>* out)
{
    out->clear();
    const int nf = (int)act.db.size();
    if (nf == 0) return;
    int f0 = std::max(0, (int)(t0 / act.hop)), f1 = std::min(nf, (int)(t1 / act.hop));
    if (f1 - f0 < 2) return;
    float on  = std::max(act.floor_db + p.on_frac * (act.peak_db - act.floor_db),
                         act.peak_db - p.max_below);
    float off = on - 3.0f;

    // Runs of singing, with hysteresis.
    std::vector<std::pair<int, int>> runs;
    bool in = false; int start = 0;
    for (int i = f0; i < f1; i++) {
        float d = act.db[i];
        if (!in && d >= on)       { in = true; start = i; }
        else if (in && d < off)   { in = false; runs.push_back({ start, i }); }
    }
    if (in) runs.push_back({ start, f1 });

    // Bridge short silences, drop blips.
    std::vector<std::pair<int, int>> merged;
    const int bridge = (int)(p.bridge_gap / act.hop);
    for (auto& r : runs) {
        if (!merged.empty() && r.first - merged.back().second < bridge) merged.back().second = r.second;
        else merged.push_back(r);
    }
    const int min_len = (int)(p.min_unit / act.hop);
    double prev_end = t0;
    for (auto& r : merged) {
        if (r.second - r.first < min_len) continue;
        std::vector<std::pair<int, float>> cuts;
        split_run(act, r.first, r.second, p, &cuts);
        int a = r.first;
        for (size_t c = 0; c <= cuts.size(); c++) {
            int b = c < cuts.size() ? cuts[c].first : r.second;
            LyricUnit u;
            u.t0 = a * act.hop;
            u.t1 = b * act.hop;
            u.gap_before = c == 0 ? std::min(5.0, u.t0 - prev_end) : 0.0;
            u.dip_before = c == 0 ? 0.0f : cuts[c - 1].second;
            u.nuclei     = count_nuclei(act, a, b);
            out->push_back(u);
            a = b;
        }
        prev_end = r.second * act.hop;
    }
}

// ---------------------------------------------------------------------------
// Alignment
// ---------------------------------------------------------------------------

namespace {

struct Ctx {
    const std::vector<LyricUnit>* u;
    std::vector<int>    next_bar;  // first unit after j that follows an anchor
    std::vector<double> bs;        // boundary strength before unit k, seconds
    std::vector<double> inner;     // prefix: inner-pause penalty of boundaries < k
    std::vector<int>    nuc;       // prefix: nuclei of units < k
    const std::vector<int>* syl;
    const LyricAlignParams* p;
    const std::vector<double>* hint = nullptr;
};

double line_cost(const Ctx& c, int i, int j, int k, double rate) {
    const LyricAlignParams& p = *c.p;
    const std::vector<LyricUnit>& u = *c.u;
    double span = u[k - 1].t1 - u[j].t0;
    double syl  = (*c.syl)[i];
    double e    = syl * rate;
    double lr   = log(span / e);
    double cost = p.w_dur * lr * lr;
    double nr   = log((c.nuc[k] - c.nuc[j] + 1.0) / (syl + 1.0));
    cost += p.w_nuc * nr * nr;
    cost += p.w_inner * (c.inner[k] - c.inner[j + 1]);
    if (j > 0) cost += p.w_break * std::max(0.0, p.break_ok - c.bs[j]);
    if (c.hint && !std::isnan((*c.hint)[i])) {
        double d = std::max(0.0, fabs(u[j].t0 - (*c.hint)[i]) - p.hint_slack) / p.hint_tol;
        cost += p.w_hint * std::min(1.0, d * d);
    }
    return cost;
}

struct Solve {
    double total;
    std::vector<int> first, last;    // unit run per line, -1 when unplaced
    std::vector<double> cost;
};

// Lines i0 .. i0+n-1 on units u0 .. m-1.  open_end: the window need not use
// up the units.  A line never spans an anchor.
Solve solve(const Ctx& c, int i0, int n, int u0, int m, double rate, bool open_end) {
    const LyricAlignParams& p = *c.p;
    const std::vector<LyricUnit>& u = *c.u;
    const double INF = 1e30;
    std::vector<double> D((size_t)(n + 1) * (m + 1), INF);
    std::vector<int>    B((size_t)(n + 1) * (m + 1), -1);   // >=0: line ran from unit B; -2: skip
    auto at = [m](int i, int j) { return (size_t)i * (m + 1) + j; };
    D[at(0, u0)] = 0.0;
    for (int i = 0; i <= n; i++) {
        for (int j = u0; j <= m; j++) {
            double d = D[at(i, j)];
            if (d >= INF) continue;
            if (j < m) {
                double s = d + p.w_skip * (u[j].t1 - u[j].t0) + p.c_skip;
                if (s < D[at(i, j + 1)]) { D[at(i, j + 1)] = s; B[at(i, j + 1)] = -2; }
            }
            if (i < n) {
                int kmax = std::min(std::min(m, j + p.max_run), c.next_bar[j]);
                for (int k = j + 1; k <= kmax; k++) {
                    double s = d + line_cost(c, i0 + i, j, k, rate);
                    if (s < D[at(i + 1, k)]) { D[at(i + 1, k)] = s; B[at(i + 1, k)] = j; }
                }
            }
        }
    }
    Solve r;
    r.first.assign(n, -1); r.last.assign(n, -1); r.cost.assign(n, 0.0);
    int best_i = 0, best_j = m; double best = INF;
    for (int i = 0; i <= n; i++) {
        double s = D[at(i, m)] + p.c_unplaced * (n - i);
        if (s < best) { best = s; best_i = i; best_j = m; }
    }
    if (open_end)
        for (int j = u0; j <= m; j++)
            if (D[at(n, j)] < best) { best = D[at(n, j)]; best_i = n; best_j = j; }
    r.total = best;
    int i = best_i, j = best_j;
    while (i > 0 || j > u0) {
        int b = B[at(i, j)];
        if (b == -2) { j--; continue; }
        if (b < 0) break;
        r.first[i - 1] = b; r.last[i - 1] = j - 1;
        r.cost[i - 1]  = line_cost(c, i0 + i - 1, b, j, rate);
        i--; j = b;
    }
    return r;
}

}  // namespace

// Cut the anchors' spans out of the units.  *barrier[k] is set when an
// anchor sits between unit k-1 and unit k.
static void carve_anchors(const LyricActivity& act, const std::vector<LyricUnit>& in,
                          const std::vector<LyricAnchor>& anchors, const LyricAlignParams& p,
                          std::vector<LyricUnit>* out, std::vector<char>* barrier)
{
    out->clear(); barrier->clear();
    double prev_end = -1e9;
    auto emit = [&](LyricUnit u, bool cut) {
        if (u.t1 - u.t0 < p.min_unit) return;
        bool bar = false;
        for (const LyricAnchor& a : anchors) {
            double mid = 0.5 * (a.t0 + a.t1);
            if (mid >= prev_end && mid <= u.t0 + 1e-9) { bar = true; break; }
        }
        if (cut) {
            int f0 = (int)(u.t0 / act.hop), f1 = (int)(u.t1 / act.hop);
            u.nuclei = f1 > f0 ? count_nuclei(act, f0, f1) : 1;
        }
        if (bar) { u.gap_before = std::max(u.gap_before, 1.0); u.dip_before = 0.0f; }
        out->push_back(u);
        barrier->push_back(bar);
        prev_end = u.t1;
    };
    for (const LyricUnit& u0 : in) {
        LyricUnit u = u0;
        bool cut = false;
        // Anchors are sorted by start: peel them off the unit left to right.
        for (const LyricAnchor& a : anchors) {
            if (a.t1 <= u.t0 || a.t0 >= u.t1) continue;
            if (a.t0 > u.t0) {
                LyricUnit l = u; l.t1 = a.t0;
                emit(l, true);
            }
            u.t0 = a.t1;
            u.gap_before = 0.0; u.dip_before = 0.0f;
            cut = true;
            if (u.t0 >= u.t1) break;
        }
        if (u.t0 < u.t1) emit(u, cut);
    }
}

namespace {

// Units of one stretch of the track, carved around the anchors, with the
// DP's precomputed sums.
struct Pass {
    std::vector<LyricUnit> units;
    std::vector<char>      barrier;
    Ctx                    c;
};

void build_pass(const LyricActivity& act, double t0, double t1,
                const std::vector<LyricAnchor>& anchors, const LyricAlignParams& p,
                const std::vector<int>* syl, const std::vector<double>* hint, Pass* ps)
{
    std::vector<LyricUnit> raw;
    lyric_units(act, t0, t1, p, &raw);
    carve_anchors(act, raw, anchors, p, &ps->units, &ps->barrier);
    const int m = (int)ps->units.size();
    Ctx& c = ps->c;
    c.u = &ps->units; c.syl = syl; c.p = &p; c.hint = hint;
    c.bs.assign(m, 0.0);
    c.inner.assign(m + 1, 0.0);
    c.nuc.assign(m + 1, 0);
    c.next_bar.assign(m + 1, m);
    for (int k = 0; k < m; k++) {
        c.bs[k] = ps->units[k].gap_before + ps->units[k].dip_before / 40.0;
        double pen = k > 0 ? std::max(0.0, c.bs[k] - p.inner_ok) : 0.0;
        c.inner[k + 1] = c.inner[k] + pen;
        c.nuc[k + 1]   = c.nuc[k] + ps->units[k].nuclei;
    }
    // inner[k] - inner[j+1] sums the boundaries before units j+1 .. k-1.
    for (int k = m - 1; k >= 0; k--)
        c.next_bar[k] = (k + 1 < m && ps->barrier[k + 1]) ? k + 1 : c.next_bar[k + 1];
}

// Lines [i_lo, i_hi) on the pass's units, a window at a time.  The rate is
// fitted on the first window of the whole alignment (*fit) and kept.
// Returns the first line not placed; *t_end gets the end of the last placed.
int run_windows(const Pass& ps, int i_lo, int i_hi, const LyricAlignParams& p,
                const std::vector<double>& rates, double* rate, bool* fit,
                double* t_end, std::vector<LyricAlignOut>* out)
{
    const int m = (int)ps.units.size();
    const int H = p.horizon > 0 ? p.horizon : (i_hi - i_lo);
    int i0 = i_lo, u0 = 0;
    while (i0 < i_hi && u0 < m) {
        int len = std::min(H, i_hi - i0);
        bool last = (i0 + len == i_hi);
        Solve best; best.total = 1e30;
        for (double r : (*fit ? rates : std::vector<double>{ *rate })) {
            Solve s = solve(ps.c, i0, len, u0, m, r, !last);
            if (s.total < best.total) { best = std::move(s); *rate = r; }
        }
        *fit = false;
        int keep = last ? len : std::max(1, len / 2);
        int next_u = -1, placed_upto = i0;
        for (int i = 0; i < keep; i++) {
            if (best.first[i] < 0) continue;
            LyricAlignOut& o = (*out)[i0 + i];
            o.placed = true;
            o.t0 = ps.units[best.first[i]].t0;
            o.t1 = ps.units[best.last[i]].t1;
            o.score = (float)exp(-best.cost[i]);
            next_u = best.last[i] + 1;
            placed_upto = i0 + i + 1;
            *t_end = o.t1;
        }
        if (next_u < 0) break;            // nothing fit: out of singing
        // A window leaves lines unplaced only at its end (the units ran
        // out): they start the next window, or are the leftovers.
        i0 = placed_upto;
        if (last) break;
        u0 = next_u;
    }
    return i0;
}

}  // namespace

double lyric_align(const LyricActivity& act, double t0, double t1,
                   const std::vector<const char*>& lines,
                   const std::vector<LyricAnchor>& anchors_in,
                   const LyricAlignParams& p, std::vector<LyricAlignOut>* out,
                   const std::vector<double>* hint)
{
    const int n = (int)lines.size();
    out->assign(n, LyricAlignOut());
    if (n == 0) return 0.0;
    std::vector<LyricAnchor> anchors = anchors_in;
    std::sort(anchors.begin(), anchors.end(),
              [](const LyricAnchor& a, const LyricAnchor& b) { return a.t0 < b.t0; });
    if (hint && (int)hint->size() != n) hint = nullptr;

    std::vector<int> syl(n);
    for (int i = 0; i < n; i++) syl[i] = std::max(1, lyric_syllables(lines[i]));

    // Singing rate: from the placed lines when there are enough, else searched.
    std::vector<double> rates;
    std::vector<double> anchor_rates;
    for (const LyricAnchor& a : anchors) {
        int s = lyric_syllables(a.text ? a.text : "");
        if (s >= 2 && a.t1 > a.t0) anchor_rates.push_back((a.t1 - a.t0) / s);
    }
    if (anchor_rates.size() >= 3) {
        std::sort(anchor_rates.begin(), anchor_rates.end());
        double rc = anchor_rates[anchor_rates.size() / 2];
        for (int g = -2; g <= 2; g++) rates.push_back(rc * pow(1.12, g));
    } else {
        const int G = 12;
        for (int g = 0; g < G; g++)
            rates.push_back(p.rate_lo * pow(p.rate_hi / p.rate_lo, g / (double)(G - 1)));
    }

    // Gaps between anchors whose place in the sheet is known: each gets
    // exactly the lines that fall between them.  (Anchors out of order with
    // the ones before them are treated as unknown.)
    struct Gap { int i_lo, i_hi; double ta, tb; };
    std::vector<Gap> gaps;
    {
        int i_prev = 0; double t_prev = t0;
        for (const LyricAnchor& a : anchors) {
            if (a.seq < i_prev || a.seq > n) continue;
            double mid = 0.5 * (a.t0 + a.t1);
            gaps.push_back({ i_prev, a.seq, t_prev, mid });
            i_prev = a.seq; t_prev = mid;
        }
        gaps.push_back({ i_prev, n, t_prev, t1 });
    }

    // Each gap on its own.  Lines left over -- the singing ran out first,
    // typically a quiet or fading last chorus under the loudness threshold
    // -- get the rest of the gap again with lower thresholds.
    static const float RELAX[] = { 0.0f, 20.0f, 30.0f };
    double rate = rates[0];
    bool fit = true;
    for (const Gap& g : gaps) {
        int i0 = g.i_lo;
        double ta = g.ta;
        for (float relax : RELAX) {
            if (i0 >= g.i_hi || ta >= g.tb) break;
            LyricAlignParams pr = p;
            if (relax > 0.0f) {
                if (relax <= p.max_below) continue;
                pr.max_below = relax;
            }
            Pass ps;
            build_pass(act, ta, g.tb, anchors, pr, &syl, hint, &ps);
            if (ps.units.empty()) continue;
            double t_end = ta;
            i0 = run_windows(ps, i0, g.i_hi, pr, rates, &rate, &fit, &t_end, out);
            ta = t_end;
        }
        // Still left over: one after another at the singing rate, as far as
        // the gap goes, with a fit score that says it is a guess.
        double t = ta + 0.3;
        for (; i0 < g.i_hi; i0++) {
            double len = std::max(1.0, syl[i0] * rate);
            if (t + len > g.tb) break;
            LyricAlignOut& o = (*out)[i0];
            o.placed = true;
            o.t0 = t; o.t1 = t + len;
            o.score = 0.05f;
            t += len + 0.3;
        }
    }
    return rate;
}

// ---------------------------------------------------------------------------
// Timed-lyrics hints
// ---------------------------------------------------------------------------

// Lowercase letters and digits, single spaces; apostrophes dropped
// ("Don't" == "dont").
static std::string norm_text(const char* t) {
    std::string o;
    bool sp = false;
    for (const unsigned char* p = (const unsigned char*)t; *p; p++) {
        if (*p == '\'' || *p == 0x60) continue;
        if (*p >= 0x80 || isalnum(*p)) {
            if (sp && !o.empty()) o += ' ';
            sp = false;
            o += (char)tolower(*p);
        } else sp = true;
    }
    return o;
}

double lyric_match_hints(const std::vector<const char*>& lines,
                         const std::vector<LyricAnchor>& anchors_in,
                         const std::vector<LyricHintLine>& hints,
                         std::vector<double>* start)
{
    start->assign(lines.size(), NAN);
    if (hints.empty() || lines.empty()) return 0.0;

    // The sheet: lines with the known-position anchors slotted in.
    struct Entry { std::string norm; int line; const LyricAnchor* anchor; };
    std::vector<const LyricAnchor*> anchors;
    for (const LyricAnchor& a : anchors_in) if (a.seq >= 0) anchors.push_back(&a);
    std::stable_sort(anchors.begin(), anchors.end(),
                     [](const LyricAnchor* x, const LyricAnchor* y) { return x->seq < y->seq; });
    std::vector<Entry> sheet;
    size_t ai = 0;
    for (size_t i = 0; i <= lines.size(); i++) {
        while (ai < anchors.size() && anchors[ai]->seq <= (int)i) {
            sheet.push_back({ norm_text(anchors[ai]->text ? anchors[ai]->text : ""), -1, anchors[ai] });
            ai++;
        }
        if (i < lines.size()) sheet.push_back({ norm_text(lines[i]), (int)i, nullptr });
    }
    std::vector<std::string> hn(hints.size());
    for (size_t h = 0; h < hints.size(); h++) hn[h] = norm_text(hints[h].text);

    // How entry e matches hint h: 0 none, 1 equal, 2 entry is the start of
    // the hint, 3 entry is the end of it.
    auto kind = [&](const std::string& e, const std::string& t) -> int {
        if (e.empty()) return 0;
        if (e == t) return 1;
        if (t.size() > e.size() + 1) {
            if (t.compare(0, e.size(), e) == 0 && t[e.size()] == ' ') return 2;
            if (t.compare(t.size() - e.size(), e.size(), e) == 0 &&
                t[t.size() - e.size() - 1] == ' ') return 3;
        }
        return 0;
    };
    auto gain = [&](const Entry& e, size_t h, int k) -> double {
        if (k == 0) return -1.0;
        double g = k == 1 ? 1.0 : 0.6;
        if (e.anchor) {
            double d = fabs(e.anchor->t0 - hints[h].t0);
            if (d > 20.0) return -1.0;               // not this chorus
            g += 1.0 - d / 20.0;
        }
        return g;
    };

    // F[e][h]: best total over the first e entries and first h hints.
    // Moves: skip an entry, skip a hint, match (both advance), or a "start"
    // match that leaves the hint for the line's next piece.
    const size_t E = sheet.size(), Hn = hints.size();
    std::vector<double> F((E + 1) * (Hn + 1), 0.0);
    std::vector<char>   how((E + 1) * (Hn + 1), 0);
    auto at = [Hn](size_t e, size_t h) { return e * (Hn + 1) + h; };
    for (size_t e = 0; e <= E; e++)
        for (size_t h = 0; h <= Hn; h++) {
            if (e == 0 && h == 0) continue;
            double best = -1e30; char b = 0;
            if (e > 0 && F[at(e - 1, h)] > best) { best = F[at(e - 1, h)]; b = 1; }   // skip entry
            if (h > 0 && F[at(e, h - 1)] > best) { best = F[at(e, h - 1)]; b = 2; }   // skip hint
            if (e > 0 && h > 0) {
                int k = kind(sheet[e - 1].norm, hn[h - 1]);
                if (k == 1 || k == 3) {
                    double g = gain(sheet[e - 1], h - 1, k);
                    if (g > 0 && F[at(e - 1, h - 1)] + g > best) { best = F[at(e - 1, h - 1)] + g; b = 3; }
                }
            }
            if (e > 0 && h < Hn) {        // start piece: hint h stays available
                int k = kind(sheet[e - 1].norm, hn[h]);
                if (k == 2 && sheet[e - 1].line >= 0) {
                    double g = gain(sheet[e - 1], h, k);
                    if (g > 0 && F[at(e - 1, h)] + g > best) { best = F[at(e - 1, h)] + g; b = 4; }
                }
            }
            F[at(e, h)] = best; how[at(e, h)] = b;
        }

    std::vector<int> match(E, -1);
    std::vector<int> mkind(E, 0);
    for (size_t e = E, h = Hn; e > 0 || h > 0;) {
        char b = how[at(e, h)];
        if (b == 1)      { e--; }
        else if (b == 2) { h--; }
        else if (b == 3) { match[e - 1] = (int)h - 1; mkind[e - 1] = kind(sheet[e - 1].norm, hn[h - 1]); e--; h--; }
        else if (b == 4) { match[e - 1] = (int)h;     mkind[e - 1] = 2; e--; }
        else break;
    }

    // Offsets measured at the matched anchors, in sheet order.  A line
    // takes the median of the (up to) three nearest before it -- recordings
    // drift, a live take more than a remaster -- or after it when none
    // precede; the global median goes back to the caller.
    std::vector<std::pair<size_t, double>> meas;
    for (size_t e = 0; e < E; e++)
        if (sheet[e].anchor && match[e] >= 0)
            meas.push_back({ e, sheet[e].anchor->t0 - hints[match[e]].t0 });
    auto median = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::vector<double> all;
    for (auto& m : meas) all.push_back(m.second);
    double offset = median(all);
    auto local_offset = [&](size_t e) {
        if (meas.empty()) return 0.0;
        size_t k = 0;
        while (k < meas.size() && meas[k].first < e) k++;   // meas[k-1] precedes e
        std::vector<double> v;
        if (k > 0) for (size_t j = k; j > 0 && v.size() < 3; j--) v.push_back(meas[j - 1].second);
        else       for (size_t j = 0; j < meas.size() && v.size() < 3; j++) v.push_back(meas[j].second);
        return median(v);
    };

    for (size_t e = 0; e < E; e++) {
        if (sheet[e].line < 0 || match[e] < 0) continue;
        size_t h = (size_t)match[e];
        double t = hints[h].t0;
        if (mkind[e] == 3) {            // last piece: as far in as the text it skips
            double end = hints[h].t1 >= 0 ? hints[h].t1
                       : h + 1 < Hn ? hints[h + 1].t0 : hints[h].t0 + 3.0;
            double frac = (double)(hn[h].size() - sheet[e].norm.size()) / hn[h].size();
            t += frac * (end - hints[h].t0);
        }
        (*start)[sheet[e].line] = t + local_offset(e);
    }
    return offset;
}
