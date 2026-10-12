#include "chart.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Lyric pacing, the same numbers as the web client (v0.4.html).
static const double CUE_MIN  = 1.2;  // a gap this long before a line earns a count-in
static const double CUE_LEAD = 4.0;  // a line moves up at most this early
static const double REST_MIN = 2.0;  // a gap this long after a line blanks the row

static const char* SECTION_KINDS[] = {
    "intro", "verse", "pre-chorus", "chorus", "post-chorus", "bridge",
    "breakdown", "instrumental", "solo", "interlude", "outro", "refrain",
};

bool read_file(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// A # starts a comment only at the start of a line or after a blank, so that
// F#m7 is a chord and not a chord with a comment.
static std::string strip_comment(const std::string& line) {
    for (size_t i = 0; i < line.size(); i++)
        if (line[i] == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t'))
            return line.substr(0, i);
    return line;
}

struct RawLine {
    std::string start, end, event;
};

static bool split_line(const std::string& raw, RawLine* out) {
    std::string line = trim(strip_comment(raw));
    if (line.empty()) return false;
    size_t a = line.find_first_of(" \t");
    if (a == std::string::npos) return false;
    size_t b = line.find_first_not_of(" \t", a);
    if (b == std::string::npos) return false;
    size_t c = line.find_first_of(" \t", b);
    if (c == std::string::npos) return false;
    size_t d = line.find_first_not_of(" \t", c);
    if (d == std::string::npos) return false;
    out->start = line.substr(0, a);
    out->end = line.substr(b, c - b);
    out->event = line.substr(d);
    return true;
}

static bool starts_with(const std::string& s, const char* p) {
    return s.compare(0, strlen(p), p) == 0;
}

// "Bx12" -> 12, "BMx4" -> 4 (with *measure set), "B" -> 1; 0 if not a beat.
static int beat_event(const std::string& ev, bool* measure) {
    std::string e = ev;
    size_t sp = e.find_first_of(" \t");
    if (sp != std::string::npos) e = e.substr(0, sp);
    *measure = false;
    if (e == "B") return 1;
    if (e == "BM") { *measure = true; return 1; }
    const char* p = nullptr;
    if (starts_with(e, "BMx")) { *measure = true; p = e.c_str() + 3; }
    else if (starts_with(e, "Bx")) p = e.c_str() + 2;
    if (!p || !*p) return 0;
    char* endp;
    long n = strtol(p, &endp, 10);
    if (*endp || n < 1 || n > 10000) return 0;
    return (int)n;
}

// A time token: seconds, or B<N> for the Nth beat (1-based).
static bool parse_time(const std::string& tok, const std::vector<double>& beats, double* out) {
    if (tok.empty()) return false;
    char* endp;
    if (tok[0] == 'B') {
        long n = strtol(tok.c_str() + 1, &endp, 10);
        if (*endp || n < 1 || n > (long)beats.size()) return false;
        *out = beats[n - 1];
        return true;
    }
    double v = strtod(tok.c_str(), &endp);
    if (*endp || !isfinite(v)) return false;
    *out = v;
    return true;
}

static std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') j++;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// "verse: 2 @3/4" -> kind verse, label "2", 3 beats a bar.
static bool parse_section(const std::string& ev, Section* s) {
    std::string head = ev, rest;
    size_t sp = ev.find_first_of(" \t:");
    if (sp != std::string::npos) { head = ev.substr(0, sp); rest = ev.substr(sp); }
    std::string kind;
    for (char c : head) kind += (char)tolower((unsigned char)c);
    bool known = false;
    for (const char* k : SECTION_KINDS) if (kind == k) known = true;
    if (!known) return false;
    if (!rest.empty() && rest[0] == ':') rest = rest.substr(1);
    s->kind = kind;
    s->beats_per_bar = 4;
    size_t at = rest.find('@');
    if (at != std::string::npos) {
        int n = atoi(rest.c_str() + at + 1);
        if (n >= 1 && n <= 16) s->beats_per_bar = n;
        size_t end = rest.find_first_of(" \t", at);
        rest = rest.substr(0, at) + (end == std::string::npos ? "" : rest.substr(end));
    }
    s->label = trim(rest);
    return true;
}

static std::string event_value(const std::string& ev, size_t keylen) {
    return trim(ev.substr(std::min(keylen, ev.size())));
}

ChartFlags chart_flags(const std::string& text) {
    ChartFlags f;
    bool licks = false, backing = false;
    size_t pos = 0;
    RawLine rl;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (!split_line(line, &rl)) continue;
        std::string tok = words(rl.event)[0];
        bool m;
        if (beat_event(tok, &m)) f.beats = true;
        else if (tok == "chord:" || tok == "chords:") f.chords = true;
        else if (tok == "lyric:" || tok == "lyric") f.lyrics = true;
        else if (starts_with(tok, "loop:")) f.loops = true;
        else if (tok == "voice:") f.melody = true;
        else if (tok == "licks" || tok == "licks:") licks = true;
        else if (tok == "backing" || tok == "backing:") backing = true;
    }
    f.trade = licks && backing;
    return f;
}

static double median_period(const std::vector<double>& beats) {
    std::vector<double> gaps;
    for (size_t i = 1; i < beats.size(); i++) {
        double g = beats[i] - beats[i - 1];
        if (g > 0.05) gaps.push_back(g);
    }
    if (gaps.empty()) return 0;
    std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
    return gaps[gaps.size() / 2];
}

Chart chart_parse(const std::string& text, double duration) {
    Chart c;
    std::vector<std::string> lines;
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos);
        lines.push_back(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? text.size() : nl + 1;
    }

    // Pass 1: the beats, since B<N> times elsewhere refer to them.
    struct Beat { double t; bool m; };
    std::vector<Beat> beats;
    RawLine rl;
    for (auto& line : lines) {
        if (!split_line(line, &rl)) continue;
        bool m;
        int n = beat_event(rl.event, &m);
        if (!n) continue;
        char* e1; char* e2;
        double t0 = strtod(rl.start.c_str(), &e1), t1 = strtod(rl.end.c_str(), &e2);
        if (*e1 || *e2 || !isfinite(t0) || !isfinite(t1) || t1 < t0) continue;
        if (n == 1) { beats.push_back({t0, m}); continue; }
        for (int i = 0; i < n; i++)
            beats.push_back({t0 + (t1 - t0) * i / (n - 1), m && i == 0});
    }
    std::sort(beats.begin(), beats.end(), [](const Beat& a, const Beat& b) { return a.t < b.t; });
    for (auto& b : beats) {
        if (!c.beats.empty() && b.t - c.beats.back() < 1e-4) {
            if (b.m) c.downbeat.back() = true;
            continue;
        }
        c.beats.push_back(b.t);
        c.downbeat.push_back(b.m);
    }

    // Pass 2: everything else.
    int gid = 0;
    for (size_t ln = 0; ln < lines.size(); ln++) {
        if (!split_line(lines[ln], &rl)) continue;
        bool m;
        if (beat_event(rl.event, &m)) continue;
        double t0, t1;
        if (!parse_time(rl.start, c.beats, &t0) || !parse_time(rl.end, c.beats, &t1) || t1 < t0) {
            char buf[64];
            snprintf(buf, sizeof buf, "line %zu: bad times", ln + 1);
            c.notes.push_back(buf);
            continue;
        }
        const std::string& ev = rl.event;
        if (starts_with(ev, "lyric:") || ev == "lyric" || starts_with(ev, "lyric ")) {
            Lyric l = {t0, t1, event_value(ev, ev[5] == ':' ? 6 : 5), 0, false, -1};
            c.lyrics.push_back(l);
        } else if (starts_with(ev, "chord:") || starts_with(ev, "chords:")) {
            auto names = words(event_value(ev, ev[5] == ':' ? 6 : 7));
            if (names.empty()) continue;
            int g = names.size() > 1 ? gid++ : -1;
            for (size_t i = 0; i < names.size(); i++) {
                double a = t0 + (t1 - t0) * i / names.size();
                double b = t0 + (t1 - t0) * (i + 1) / names.size();
                c.chords.push_back({a, b, names[i], g});
            }
        } else if (starts_with(ev, "tuning:")) {
            std::string v = event_value(ev, 7);
            if (!v.empty() && (v[0] == 'A' || v[0] == 'a')) v = v.substr(1);
            double hz = atof(v.c_str());
            if (!c.tuning_from_map && hz >= 415 && hz <= 466) {
                c.tuning = hz;
                c.tuning_from_map = true;
            }
        } else if (starts_with(ev, "loop:")) {
            std::string name = event_value(ev, 5);
            if (name.empty()) name = "loop " + std::to_string(c.loops.size() + 1);
            c.loops.push_back({t0, t1, name});
        } else {
            Section s;
            if (parse_section(ev, &s)) {
                s.t0 = t0; s.t1 = t1;
                c.sections.push_back(s);
            }
            // Anything else (voice:, strum:, key:, licks, play:, ...) is a
            // layer this client does not draw, and is ignored.
        }
    }

    auto by_start = [](const auto& a, const auto& b) { return a.t0 < b.t0 || (a.t0 == b.t0 && a.t1 < b.t1); };
    std::stable_sort(c.lyrics.begin(), c.lyrics.end(), by_start);
    std::stable_sort(c.chords.begin(), c.chords.end(), by_start);
    std::stable_sort(c.sections.begin(), c.sections.end(), by_start);
    std::stable_sort(c.loops.begin(), c.loops.end(), by_start);

    // Lyric pacing.
    for (size_t i = 0; i < c.lyrics.size(); i++) {
        Lyric& l = c.lyrics[i];
        double prev_end = i ? c.lyrics[i - 1].t1 : 0;
        l.cued = i == 0 ? l.t0 >= CUE_MIN : l.t0 - prev_end >= CUE_MIN;
        l.promote = i == 0 ? 0 : std::max(prev_end, l.t0 - CUE_LEAD);
        if (l.promote > l.t0) l.promote = l.t0;
    }
    for (size_t i = 0; i < c.lyrics.size(); i++) {
        Lyric& l = c.lyrics[i];
        double next = i + 1 < c.lyrics.size() ? c.lyrics[i + 1].promote : 1e30;
        l.rest = next - l.t1 >= REST_MIN ? l.t1 : -1;
    }

    c.period = median_period(c.beats);
    if (c.period <= 0) c.period = 0.5;
    chart_build_bars(&c, duration);
    return c;
}

// --- the beat grid ----------------------------------------------------------

double Chart::beat_pos(double t) const {
    if (beats.empty()) return t / period;
    if (t <= beats.front()) return (t - beats.front()) / period;
    if (t >= beats.back()) return (beats.size() - 1) + (t - beats.back()) / period;
    size_t i = std::upper_bound(beats.begin(), beats.end(), t) - beats.begin() - 1;
    double gap = beats[i + 1] - beats[i];
    return i + (gap > 0 ? (t - beats[i]) / gap : 0);
}

double Chart::beat_time(double b) const {
    if (beats.empty()) return b * period;
    if (b <= 0) return beats.front() + b * period;
    double last = beats.size() - 1;
    if (b >= last) return beats.back() + (b - last) * period;
    size_t i = (size_t)b;
    return beats[i] + (beats[i + 1] - beats[i]) * (b - i);
}

int Chart::lyric_at(double t) const {
    int lo = -1;
    for (size_t i = 0; i < lyrics.size() && lyrics[i].promote <= t; i++) lo = (int)i;
    return lo;
}

int Chart::bar_at(double t) const {
    if (bars.empty() || t < bars.front().t0) return -1;
    auto it = std::upper_bound(bars.begin(), bars.end(), t,
                               [](double v, const Bar& b) { return v < b.t0; });
    return (int)(it - bars.begin()) - 1;
}

int Chart::block_of(int bar) const {
    for (size_t i = 0; i < blocks.size(); i++)
        if (bar >= blocks[i].bar0 && bar < blocks[i].bar0 + blocks[i].nbars) return (int)i;
    return -1;
}

int Chart::section_at(double t) const {
    int hit = -1;
    for (size_t i = 0; i < sections.size(); i++)
        if (t >= sections[i].t0 && t < sections[i].t1) hit = (int)i;
    return hit;
}

// --- bars -------------------------------------------------------------------
//
// A bar is counted in beats from the last anchor -- a moment known to be a
// downbeat.  Anchors, strongest first:
//   - BM beats, when the map has them (none of today's do);
//   - section starts, which a person put on the one;
//   - the start of each chords: line, which the chord tool writes a bar per
//     name, so each name's start is a downbeat.
// Between anchors bars are beats_per_bar long, and a bar that runs into the
// next anchor is cut short there: a pickup, or a section mark a beat late,
// costs one short bar instead of shifting every bar after it.

void chart_build_bars(Chart* c, double duration) {
    c->bars.clear();
    c->blocks.clear();
    if (c->beats.empty()) {
        if (c->chords.empty()) return;
        // No beats: guess a grid from the chords so the chord row still pages.
        // A chords: line is a bar per name; failing that, a chord per 4 beats.
        std::vector<double> bar_lens;
        for (auto& ch : c->chords)
            if (ch.group >= 0) bar_lens.push_back(ch.t1 - ch.t0);
        double bar = 2.0;
        if (!bar_lens.empty()) {
            std::nth_element(bar_lens.begin(), bar_lens.begin() + bar_lens.size() / 2, bar_lens.end());
            bar = bar_lens[bar_lens.size() / 2];
        }
        c->period = bar / 4;
        c->synthetic_beats = true;
        double end = duration > 0 ? duration : c->chords.back().t1;
        for (double t = fmod(c->chords.front().t0, c->period); t <= end + c->period; t += c->period) {
            c->beats.push_back(t);
            c->downbeat.push_back(false);
        }
    }
    if (duration <= 0) {
        duration = c->beats.back();
        if (!c->chords.empty()) duration = std::max(duration, c->chords.back().t1);
        if (!c->lyrics.empty()) duration = std::max(duration, c->lyrics.back().t1);
    }

    auto snap = [&](double t) { return floor(c->beat_pos(t) + 0.5); };
    std::vector<double> anchors;
    for (size_t i = 0; i < c->beats.size(); i++)
        if (c->downbeat[i]) anchors.push_back((double)i);
    std::vector<double> section_starts;
    for (auto& s : c->sections) { anchors.push_back(snap(s.t0)); section_starts.push_back(snap(s.t0)); }
    for (auto& ch : c->chords)
        if (ch.group >= 0) anchors.push_back(snap(ch.t0));
    if (anchors.empty()) anchors.push_back(c->chords.empty() ? 0 : snap(c->chords.front().t0));
    std::sort(anchors.begin(), anchors.end());
    anchors.erase(std::unique(anchors.begin(), anchors.end()), anchors.end());
    std::sort(section_starts.begin(), section_starts.end());

    auto meter_at = [&](double b) {
        double t = c->beat_time(b + 0.25);
        int s = c->section_at(t);
        if (s >= 0) return c->sections[s].beats_per_bar;
        return c->sections.empty() ? 4 : c->sections.front().beats_per_bar;
    };

    // Bars before the first anchor are counted back from it to time zero.
    double first = anchors.front();
    int n0 = meter_at(first);
    double start = c->beat_pos(0);
    double b = first - n0 * ceil((first - start) / n0 - 1e-9);
    size_t next_anchor = 0;
    int number = 1;
    while (c->beat_time(b) < duration - 0.01) {
        while (next_anchor < anchors.size() && anchors[next_anchor] <= b + 1e-9) next_anchor++;
        double e = b + meter_at(b);
        if (next_anchor < anchors.size() && e > anchors[next_anchor]) e = anchors[next_anchor];
        // A sliver of a bar before the first anchor is not a bar anyone counts.
        if (c->bars.empty() && c->beat_time(e) - std::max(0.0, c->beat_time(b)) < c->period * 1.5) {
            b = e;
            continue;
        }
        Bar bar;
        bar.b0 = b; bar.b1 = e;
        bar.t0 = c->beat_time(b); bar.t1 = c->beat_time(e);
        bar.section = c->section_at(c->beat_time(b + 0.25));
        bar.number = number++;
        c->bars.push_back(bar);
        b = e;
        if (c->bars.size() > 20000) break;  // a corrupt map should not hang us
    }

    // Chords onto bars, by beat.  A change that lands within a quarter beat
    // of a beat is on that beat; one that overlaps a bar by less than half a
    // beat is a sloppy edge, not a chord in that bar.
    for (auto& bar : c->bars) {
        for (auto& ch : c->chords) {
            double cb0 = c->beat_pos(ch.t0), cb1 = c->beat_pos(ch.t1);
            double lo = std::max(cb0, bar.b0), hi = std::min(cb1, bar.b1);
            if (hi - lo < 0.5 && !(cb1 - cb0 < 0.5 && cb0 >= bar.b0 && cb0 < bar.b1)) continue;
            BarChord bc;
            bc.carried = cb0 < bar.b0 - 0.25;
            bc.at = bc.carried ? 0 : std::max(0.0, floor((cb0 - bar.b0) * 2 + 0.5) / 2);
            bc.name = ch.name;
            if (!bar.chords.empty() && bar.chords.back().name == bc.name) continue;
            if (!bar.chords.empty() && bar.chords.back().at >= bc.at) bar.chords.back() = bc;
            else bar.chords.push_back(bc);
        }
    }

    // Blocks of four, restarting at each section so a verse never begins in
    // the middle of a row.  With no sections, chords: lines are the next best
    // thing to a phrase boundary.
    std::vector<double> resets = section_starts;
    if (resets.empty())
        for (auto& ch : c->chords)
            if (ch.group >= 0 && (resets.empty() || resets.back() != snap(ch.t0)))
                resets.push_back(snap(ch.t0));
    std::sort(resets.begin(), resets.end());
    size_t ri = 0;
    for (size_t i = 0; i < c->bars.size(); i++) {
        bool reset = false;
        while (ri < resets.size() && resets[ri] < c->bars[i].b1 - 1e-9) {
            if (resets[ri] >= c->bars[i].b0 - 1e-9) reset = true;
            ri++;
        }
        if (c->blocks.empty() || reset || c->blocks.back().nbars == 4)
            c->blocks.push_back({(int)i, 1});
        else
            c->blocks.back().nbars++;
    }
}
