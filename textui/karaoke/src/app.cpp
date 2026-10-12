#include "app.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "chordfont.h"

double wall_clock() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static std::string fmt_time(double t) {
    if (t < 0) t = 0;
    int s = (int)t;
    char buf[32];
    snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

static double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static double arc(double p) { return 4 * p * (1 - p); }  // one hop, 0 at both ends

// A string cut to n display columns, with an ellipsis if it was longer.
// 432, 432.5: an A4 as a tuning line would write it.
static std::string fmt_hz(double hz) {
    char buf[16];
    snprintf(buf, sizeof buf, "%.1f", hz);
    std::string r = buf;
    if (r.size() > 2 && r.compare(r.size() - 2, 2, ".0") == 0) r.resize(r.size() - 2);
    return r;
}

static std::string fit(const std::string& s, int n) {
    if (n <= 0) return "";
    if (utf8_len(s) <= n) return s;
    return s.substr(0, utf8_offset(s, n - 1)) + "…";
}

static std::string pad(const std::string& s, int n) {
    std::string o = fit(s, n);
    int len = utf8_len(o);
    if (len < n) o.append(n - len, ' ');
    return o;
}

// --- styles ---------------------------------------------------------------

static const Style ST_TEXT    = S(C_DEFAULT);
static const Style ST_DIM     = S(C_GRAY);
static const Style ST_TITLE   = S(C_BWHITE, A_BOLD);
static const Style ST_ACCENT  = S(C_BCYAN, A_BOLD);
static const Style ST_SUNG    = S(C_BYELLOW, A_BOLD);
static const Style ST_TO_SING = S(C_BWHITE, A_BOLD);
static const Style ST_NEXT    = S(C_WHITE);
static const Style ST_SQUARE  = S(C_BMAGENTA, A_BOLD);
static const Style ST_BOX     = S(C_GRAY);
static const Style ST_BOX_NOW = S(C_BYELLOW, A_BOLD);
static const Style ST_CHORD   = S(C_WHITE);
static const Style ST_CHORD_NOW = S(C_BYELLOW, A_BOLD);
static const Style ST_LOOP    = S(C_BMAGENTA);
static const Style ST_KEY     = S(C_BCYAN);
static const Style ST_SELECT  = S(C_BWHITE, A_BOLD, C_BLUE);

// --- the clock --------------------------------------------------------------

double App::now() const { return audio ? player.position() : fake_time; }
bool   App::playing() const { return audio && player.playing(); }

double App::duration() const {
    if (audio && player.ready) return player.duration();
    double d = 0;
    if (!chart.bars.empty()) d = chart.bars.back().t1;
    if (!chart.lyrics.empty()) d = std::max(d, chart.lyrics.back().t1);
    return d;
}

void App::say(const std::string& m) {
    message = m;
    message_until = wall_clock() + 3;
}

// --- loading ----------------------------------------------------------------

bool App::open_track(int index, double start_at, bool play) {
    if (index < 0 || index >= (int)library.tracks.size()) return false;
    Track& t = library.tracks[index];
    load_error.clear();
    track = index;

    double dur = 0;
    if (audio) {
        if (!player.load(t.media)) {
            load_error = "could not decode " + t.media;
            have_chart = false;
            chart = Chart();
            return false;
        }
        dur = player.duration();
    }
    chart = Chart();
    have_chart = false;
    if (!t.chart.empty()) {
        std::string text;
        if (read_file(t.chart, &text)) {
            chart = chart_parse(text, dur);
            have_chart = true;
            for (auto& n : chart.notes) fprintf(stderr, "[chart] %s: %s\n", t.title.c_str(), n.c_str());
        }
    }
    recents_touch(t.media);
    t.last = wall_clock();
    if (audio) {
        if (start_at > 0) player.seek(start_at);
        if (play) player.toggle();
    } else {
        fake_time = start_at;
    }
    return true;
}

// --- drawing: the main screen ----------------------------------------------

void App::draw(Screen& s) {
    s.clear();
    if (picker.open) draw_picker(s);
    else draw_main(s);
}

void App::draw_header(Screen& s) {
    double t = now(), d = duration();
    std::string title = track >= 0 ? library.tracks[track].title : "no track";
    std::string glyph = playing() ? "▶" : "■";
    s.text(1, 0, glyph, playing() ? ST_ACCENT : ST_DIM);
    std::string when = fmt_time(t) + " / " + fmt_time(d);
    int right = s.w - 1 - utf8_len(when);
    s.text(3, 0, fit(title, right - 5), ST_TITLE);
    s.text(right, 0, when, ST_TEXT);

    // The timeline: played, unplayed, and the loops being played.
    int x0 = 1, n = s.w - 2;
    if (n < 4) return;
    for (int i = 0; i < n; i++) {
        double ti = d > 0 ? d * (i + 0.5) / n : 0;
        bool played = ti <= t;
        bool in_loop = false;
        for (auto& e : player.programme) if (ti >= e.from && ti < e.to) in_loop = true;
        Style st = in_loop ? ST_LOOP : played ? ST_ACCENT : ST_DIM;
        s.put(x0 + i, 1, played ? "━" : "─", 3, st);
    }
    for (auto& sec : chart.sections) {
        int x = d > 0 ? (int)(sec.t0 / d * n) : 0;
        if (x >= 0 && x < n) s.put(x0 + x, 1, "┃", 3, sec.t0 <= t ? ST_ACCENT : ST_DIM);
    }

    // What is where: section, bar, tempo, and the tuning controls.
    std::string info;
    int sec = chart.section_at(t);
    if (sec >= 0) {
        info += chart.sections[sec].kind;
        if (!chart.sections[sec].label.empty()) info += " " + chart.sections[sec].label;
        info += "   ";
    }
    int bar = chart.bar_at(t);
    if (bar >= 0) info += "bar " + std::to_string(chart.bars[bar].number) + "   ";
    if (!chart.beats.empty() && !chart.synthetic_beats) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.0f bpm   ", chart.bpm());
        info += buf;
    }
    s.text(1, 2, info, ST_DIM);

    char tune[96];
    snprintf(tune, sizeof tune, "speed %.2f×  key %+d st %+d¢  A%s",
             player.speed, player.semitones, player.cents, fmt_hz(chart.tuning).c_str());
    bool tuned = player.speed != 1.0f || player.semitones || player.cents;
    s.text(s.w - 1 - utf8_len(tune), 2, tune, tuned ? ST_ACCENT : ST_DIM);
}

// The lyric rows, with the square riding the current line.  Returns the rows
// used.  rows: how many are available.
int App::draw_lyrics(Screen& s, int top, int rows) {
    const auto& L = chart.lyrics;
    if (L.empty() || rows < 3) return 0;
    double t = now();
    int li = chart.lyric_at(t);
    bool resting = li >= 0 && L[li].rest >= 0 && t >= L[li].rest;
    int cur = resting ? -1 : li;
    int next0 = li + 1;

    // Rows, top down: two for the square, current, then what is next.  A line
    // once sung is gone; the screen is for what is coming.
    int y = top;
    int avail = s.w - 4;
    int sq_hi = y, sq_lo = y + 1, line_y = y + 2;
    y += 3;
    int n_next = std::min(rows - (y - top), 6);
    for (int k = 0; k < n_next; k++) {
        int i = next0 + k;
        if (i >= (int)L.size()) break;
        std::string p = fit(L[i].text, avail);
        s.text((s.w - utf8_len(p)) / 2, y + k, p, k == 0 ? ST_NEXT : ST_DIM);
    }

    if (cur < 0) return y - top + n_next;

    const Lyric& l = L[cur];
    std::string text = l.text;
    int len = utf8_len(text);
    double f = l.t1 > l.t0 ? clamp01((t - l.t0) / (l.t1 - l.t0)) : (t >= l.t0 ? 1 : 0);

    // Where the square is, in columns from the start of the text, and how far
    // up it is: 0 sitting on the line, 3 at the top of its hop.
    double sq_x;
    double height = 0;
    double bnow = chart.beat_pos(t);
    if (t < l.t0) {
        // Count-in: up to four beats, a dot a beat, the square hopping along
        // them and landing on the first syllable on time.
        double b = chart.beat_pos(l.t0) - bnow;
        if (l.cued && b <= 4.0) {
            sq_x = -2 * b;
            height = arc(b - floor(b));
        } else {
            sq_x = l.cued ? -8 : 0;
            height = l.cued ? 0 : arc(bnow - floor(bnow)) * 0.5;
        }
    } else if (t <= l.t1) {
        sq_x = f * (len > 1 ? len - 1 : 0);
        height = arc(bnow - floor(bnow));
    } else {
        sq_x = len > 0 ? len - 1 : 0;
    }

    // A line wider than the screen scrolls so the square stays in view.
    int x0 = (s.w - len) / 2;
    if (len > avail) {
        int off = (int)sq_x - avail / 2;
        off = std::max(0, std::min(off, len - avail));
        x0 = 2 - off;
    }

    int sung = t < l.t0 ? 0 : (int)floor(f * len + 0.5);
    size_t cut = utf8_offset(text, sung);
    int xe = s.text(x0, line_y, text.substr(0, cut), ST_SUNG);
    s.text(xe, line_y, text.substr(cut), ST_TO_SING);
    // Keep the scrolled line off the margins.
    if (len > avail) {
        s.fill(0, line_y, 2, " ", ST_TEXT);
        s.fill(s.w - 2, line_y, 2, " ", ST_TEXT);
        if (x0 < 2) s.put(1, line_y, "…", 3, ST_DIM);
        if (x0 + len > s.w - 2) s.put(s.w - 2, line_y, "…", 3, ST_DIM);
    }

    if (t < l.t0 && l.cued) {
        double b = chart.beat_pos(l.t0) - bnow;
        for (int k = 1; k <= 4; k++)
            if (k < b + 0.5) s.put(x0 - 2 * k, line_y, "•", 3, ST_DIM);
    }

    // The square: a half block, which on a cell twice as tall as it is wide
    // is square, at one of four heights across the two rows above the line.
    int col = x0 + (int)floor(sq_x + 0.5);
    int level = (int)floor(height * 3 + 0.5);
    if (col >= 0 && col < s.w) {
        int row = level >= 2 ? sq_hi : sq_lo;
        s.put(col, row, (level & 1) ? "▀" : "▄", 3, ST_SQUARE);
    }
    return y - top + n_next;
}

// --- chords -----------------------------------------------------------------

static int glyph_w(char ch) {
    int i = (unsigned char)ch - CHORD_FONT_BASE;
    if (i < 0 || i >= (int)(sizeof CHORD_FONT_W / sizeof CHORD_FONT_W[0])) return 0;
    return CHORD_FONT_W[i];
}

static int art_width(const std::string& name) {
    int w = 0;
    for (size_t i = 0; i < name.size(); i++) w += (i ? 1 : 0) + glyph_w(name[i]);
    return w;
}

static void put_art(Screen& s, int x, int y, const std::string& name, Style st) {
    for (int row = 0; row < CHORD_FONT_H; row++) {
        int cx = x;
        for (size_t i = 0; i < name.size(); i++) {
            if (i) cx++;
            int gi = (unsigned char)name[i] - CHORD_FONT_BASE;
            if (gi < 0 || gi >= (int)(sizeof CHORD_FONT / sizeof CHORD_FONT[0])) continue;
            s.text(cx, y + row, CHORD_FONT[gi][row], st);
            cx += glyph_w(name[i]);
        }
    }
}

static bool no_chord(const std::string& n) {
    return n == "N" || n == "NC" || n == "N.C." || n == "X" || n == "-";
}

// Box border, single or double.
static void draw_box(Screen& s, int x, int y, int w, int h, bool dbl, Style st) {
    const char *tl = dbl ? "╔" : "┌", *tr = dbl ? "╗" : "┐", *bl = dbl ? "╚" : "└",
               *br = dbl ? "╝" : "┘", *hz = dbl ? "═" : "─", *vt = dbl ? "║" : "│";
    s.put(x, y, tl, 3, st);
    s.fill(x + 1, y, w - 2, hz, st);
    s.put(x + w - 1, y, tr, 3, st);
    for (int r = 1; r < h - 1; r++) {
        s.put(x, y + r, vt, 3, st);
        s.put(x + w - 1, y + r, vt, 3, st);
    }
    s.put(x, y + h - 1, bl, 3, st);
    s.fill(x + 1, y + h - 1, w - 2, hz, st);
    s.put(x + w - 1, y + h - 1, br, 3, st);
}

static const int CHORD_ROWS = 7;  // playhead, box (5), preview

void App::draw_chords(Screen& s, int top) {
    if (chart.bars.empty()) return;
    double t = now();
    int bi = chart.bar_at(t);
    if (bi < 0) bi = 0;
    int blk = chart.block_of(bi);
    if (blk < 0) return;
    const Block& B = chart.blocks[blk];
    bool have_chords = !chart.chords.empty();

    int margin = 2, gap = 2;
    int box_w = std::min((s.w - 2 * margin - 3 * gap) / 4, 30);
    if (box_w < 8) return;
    int total = 4 * box_w + 3 * gap;
    int left = (s.w - total) / 2;
    int box_y = top + 1;

    for (int k = 0; k < B.nbars; k++) {
        const Bar& bar = chart.bars[B.bar0 + k];
        int x = left + k * (box_w + gap);
        bool now_bar = B.bar0 + k == bi && t >= bar.t0;
        bool done = t >= bar.t1;
        draw_box(s, x, box_y, box_w, 5, now_bar, now_bar ? ST_BOX_NOW : ST_BOX);
        std::string num = " " + std::to_string(bar.number) + " ";
        s.text(x + 2, box_y, num, now_bar ? ST_BOX_NOW : ST_DIM);

        int inner = box_w - 2, ix = x + 1, iy = box_y + 1;
        double beats = bar.b1 - bar.b0;
        double pos = chart.beat_pos(t) - bar.b0;

        if (!have_chords) {
            // No chords, but a beat grid: the bar number is the landmark.
            std::string n = std::to_string(bar.number);
            put_art(s, ix + (inner - art_width(n)) / 2, iy, n, now_bar ? ST_CHORD_NOW : ST_DIM);
        } else if (bar.chords.empty()) {
            s.text(ix + inner / 2, iy + 1, "·", ST_DIM);
        } else {
            // Each chord at its beat, if they fit there; packed left to right
            // if they do not; as plain text if even that does not fit.
            std::vector<int> xs, ws;
            bool art = true;
            for (auto& c : bar.chords) {
                ws.push_back(no_chord(c.name) ? 4 : art_width(c.name));
                xs.push_back((int)floor(c.at / beats * inner + 0.5) + 1);
            }
            for (size_t i = 0; i < xs.size(); i++) {
                if (i && xs[i] < xs[i - 1] + ws[i - 1] + 1) xs[i] = xs[i - 1] + ws[i - 1] + 1;
            }
            if (!xs.empty() && xs.back() + ws.back() > inner) {
                int sum = -1;
                for (int w : ws) sum += w + 1;
                if (sum + 1 <= inner) {
                    int cx = std::max(0, (inner - sum) / 2);
                    for (size_t i = 0; i < xs.size(); i++) { xs[i] = cx; cx += ws[i] + 1; }
                } else {
                    art = false;
                }
            }
            if (bar.chords.size() == 1 && art)
                xs[0] = std::max(1, (inner - ws[0]) / 2);  // a whole bar of one chord sits centred
            for (size_t i = 0; i < bar.chords.size(); i++) {
                const BarChord& c = bar.chords[i];
                bool sounding = now_bar && pos >= c.at &&
                                (i + 1 == bar.chords.size() || pos < bar.chords[i + 1].at);
                Style st = sounding ? ST_CHORD_NOW : done ? ST_DIM : ST_CHORD;
                if (!art) continue;
                if (no_chord(c.name)) s.text(ix + xs[i], iy + 1, "N.C.", st);
                else put_art(s, ix + xs[i], iy, c.name, st);
            }
            if (!art) {
                std::string line;
                for (auto& c : bar.chords) line += (line.empty() ? "" : " ") + c.name;
                line = fit(line, inner);
                s.text(ix + (inner - utf8_len(line)) / 2, iy + 1, line, now_bar ? ST_CHORD_NOW : ST_CHORD);
            }
        }

        // The playhead, riding over the bar it is in.
        if (now_bar) {
            int hx = ix + (int)floor(clamp01(pos / beats) * (inner - 1) + 0.5);
            s.put(hx, top, "▼", 3, ST_BOX_NOW);
        }
    }

    // What comes after this block, small.
    int py = box_y + 5;
    if (blk + 1 < (int)chart.blocks.size()) {
        const Block& N = chart.blocks[blk + 1];
        std::string line = "next ";
        const Bar& first = chart.bars[N.bar0];
        if (first.section >= 0 && first.section != chart.bars[B.bar0].section)
            line += chart.sections[first.section].kind + " ";
        for (int k = 0; k < N.nbars; k++) {
            const Bar& bar = chart.bars[N.bar0 + k];
            std::string names;
            for (auto& c : bar.chords) names += (names.empty() ? "" : " ") + c.name;
            if (names.empty()) names = have_chords ? "·" : std::to_string(bar.number);
            line += "│ " + pad(names, std::max(3, box_w / 2 - 2)) + " ";
        }
        line += "│";
        s.text(left, py, fit(line, s.w - left - 1), ST_DIM);
    }
}

void App::draw_footer(Screen& s) {
    int y = s.h - 1;
    // Loops: the chart's, numbered for the keys, and the programme playing.
    if (!chart.loops.empty() && s.h >= 12) {
        int ly = y - 1;
        int x = s.text(1, ly, "loops ", ST_DIM);
        for (size_t i = 0; i < chart.loops.size() && i < 9; i++) {
            char k[4];
            snprintf(k, sizeof k, "%zu", i + 1);
            x = s.text(x, ly, k, ST_KEY);
            x = s.text(x + 1, ly, chart.loops[i].name, ST_TEXT) + 2;
        }
        if (player.looping()) {
            x = s.text(x + 1, ly, "⟳ ", ST_LOOP);
            for (size_t i = 0; i < player.programme.size(); i++) {
                if (i) x = s.text(x, ly, " ▸ ", ST_DIM);
                x = s.text(x, ly, player.programme[i].name,
                           (int)i == player.current ? S(C_BWHITE, A_BOLD, C_MAGENTA) : ST_LOOP);
            }
        }
    }

    if (!message.empty() && wall_clock() < message_until) {
        s.text(1, y, fit(message, s.w - 2), ST_ACCENT);
        return;
    }
    struct { const char* k; const char* what; } keys[] = {
        {"space", "play/stop"}, {"←→", "5s"}, {"l", "load"}, {"1-9", "loop"},
        {"o", "clear loops"}, {"⌫", "drop loop"}, {"home", "start"},
        {"-=", "speed"}, {"[]", "key"}, {";'", "cents"}, {"u", "A440"}, {"q", "quit"},
    };
    int x = 1;
    for (auto& k : keys) {
        int need = utf8_len(k.k) + 1 + (int)strlen(k.what) + 2;
        if (x + need > s.w - 1) break;
        x = s.text(x, y, k.k, ST_KEY);
        x = s.text(x + 1, y, k.what, ST_DIM) + 2;
    }
}

void App::draw_main(Screen& s) {
    draw_header(s);
    if (track < 0) {
        s.text(2, 4, "No track.  Press l to load one.", ST_DIM);
        draw_footer(s);
        return;
    }
    if (!load_error.empty()) s.text(2, 4, fit(load_error, s.w - 4), S(C_BRED));
    else if (!have_chart) s.text(2, 4, "No chart for this track: just the audio.", ST_DIM);

    bool chords = !chart.bars.empty() && s.h >= 16;
    int footer = (!chart.loops.empty() && s.h >= 12) ? 2 : 1;
    int top = 4;
    int bottom = s.h - footer - (chords ? CHORD_ROWS + 1 : 0);
    int rows = bottom - top;
    if (!chart.lyrics.empty()) {
        // Centre the lyric block in the room it has.
        int want = std::min(rows, 3 + 5);
        int pad_top = std::max(0, (rows - want) / 2);
        draw_lyrics(s, top + pad_top, std::min(rows, want + (rows - want - pad_top)));
    } else if (have_chart && rows > 2) {
        s.text(2, top + rows / 2, "no lyrics in this chart", ST_DIM);
    }
    if (chords) draw_chords(s, bottom);
    draw_footer(s);
}

// --- the load dialog ---------------------------------------------------------

static const char* FILTER_NAME[N_FILTERS] = {"lyrics", "chords", "beats", "loops", "melody", "trade"};
static const char  FILTER_KEY[N_FILTERS]  = {'L', 'D', 'B', 'O', 'V', 'T'};

static bool flag(const ChartFlags& f, int i) {
    switch (i) {
    case F_LYRICS: return f.lyrics;
    case F_CHORDS: return f.chords;
    case F_BEATS:  return f.beats;
    case F_LOOPS:  return f.loops;
    case F_MELODY: return f.melody;
    case F_TRADE:  return f.trade;
    }
    return false;
}

bool App::passes(const Track& t) const {
    for (int i = 0; i < N_FILTERS; i++)
        if (picker.filters[i] && !flag(t.has, i)) return false;
    return true;
}

static std::string row_key(const PickRow& r) {
    return r.track >= 0 ? "t" + std::to_string(r.track) : "l" + std::to_string(r.list);
}

static int track_score(const std::string& q, const Track& t) {
    if (q.empty()) return 0;
    int a = fuzzy_score(q, t.title);
    int b = fuzzy_score(q, t.folder.empty() ? t.source : t.folder + " " + t.source);
    return std::max(a, b >= 0 ? b / 2 : -1);   // the title is what people type
}

void App::refilter() {
    Picker& p = picker;
    p.rows.clear();
    auto sorted = [&](std::vector<int> idx, bool keep_order) {
        std::vector<std::pair<int, int>> scored;
        for (int i : idx) {
            const Track& t = library.tracks[i];
            if (!passes(t)) continue;
            int sc = track_score(p.query, t);
            if (sc < 0) continue;
            scored.push_back({sc, i});
        }
        if (!keep_order || !p.query.empty())
            std::stable_sort(scored.begin(), scored.end(), [&](auto& a, auto& b) {
                if (a.first != b.first) return a.first > b.first;
                const Track &x = library.tracks[a.second], &y = library.tracks[b.second];
                if (x.last != y.last) return x.last > y.last;
                return keep_order ? false : x.title < y.title;
            });
        std::vector<int> out;
        for (auto& s : scored) out.push_back(s.second);
        return out;
    };

    if (!p.lists_tab) {
        std::vector<int> all(library.tracks.size());
        for (size_t i = 0; i < all.size(); i++) all[i] = (int)i;
        for (int i : sorted(all, false)) { PickRow r; r.track = i; p.rows.push_back(r); }
    } else if (p.open_list >= 0) {
        for (int i : sorted(library.lists[p.open_list].tracks, true)) { PickRow r; r.track = i; p.rows.push_back(r); }
    } else {
        // A query matching a list's name shows the list; one matching tracks
        // inside a list shows those tracks under it.
        for (size_t li = 0; li < library.lists.size(); li++) {
            const TrackList& l = library.lists[li];
            bool name_hit = p.query.empty() || fuzzy_score(p.query, l.name) >= 0;
            std::vector<int> hits;
            if (!name_hit) hits = sorted(l.tracks, true);
            if (!name_hit && hits.empty()) continue;
            PickRow h; h.list = (int)li; p.rows.push_back(h);
            for (int i : hits) { PickRow r; r.track = i; p.rows.push_back(r); }
        }
    }

    // The cursor stays on what it was on, if that is still listed.
    p.cursor = 0;
    for (size_t i = 0; i < p.rows.size(); i++)
        if (row_key(p.rows[i]) == p.cursor_key) p.cursor = (int)i;
    if (!p.rows.empty()) p.cursor_key = row_key(p.rows[p.cursor]);
}

void App::open_picker() {
    picker.open = true;
    if (track >= 0) picker.cursor_key = "t" + std::to_string(track);
    refilter();
}

void App::draw_picker(Screen& s) {
    Picker& p = picker;
    int y = 0;
    s.text(1, y, "Load", ST_TITLE);
    std::string where;
    const char* home = getenv("HOME");
    for (auto r : library.roots) {
        if (home && *home && r.compare(0, strlen(home), home) == 0) r = "~" + r.substr(strlen(home));
        where += (where.empty() ? "" : "  ") + r;
    }
    s.text(7, y, fit(where, s.w - 8), ST_DIM);
    y++;

    // Tabs, then the filters.
    int x = s.text(1, y, " Tracks ", !p.lists_tab ? ST_SELECT : ST_DIM);
    x = s.text(x + 1, y, " Lists ", p.lists_tab ? ST_SELECT : ST_DIM);
    x = s.text(x + 1, y, "tab", ST_KEY) + 3;
    for (int i = 0; i < N_FILTERS; i++) {
        char k[4] = {'^', FILTER_KEY[i], 0};
        if (x + 3 + (int)strlen(FILTER_NAME[i]) >= s.w) break;
        x = s.text(x, y, k, ST_KEY);
        x = s.text(x + 1, y, FILTER_NAME[i], p.filters[i] ? S(C_BGREEN, A_BOLD | A_UNDERLINE) : ST_DIM) + 2;
    }
    y++;

    std::string prompt = p.open_list >= 0 && p.lists_tab ? library.lists[p.open_list].name + " › " : "› ";
    x = s.text(1, y, prompt, ST_ACCENT);
    x = s.text(x, y, p.query, ST_TITLE);
    s.put(x, y, "█", 3, ST_ACCENT);
    int ntracks = 0;
    for (auto& r : p.rows) ntracks += r.track >= 0;
    std::string count = p.lists_tab && p.open_list < 0
        ? std::to_string(library.lists.size()) + " lists"
        : (p.query.empty() && !std::count(p.filters, p.filters + N_FILTERS, true) && !p.lists_tab
               ? std::to_string(library.tracks.size()) + " tracks"
               : std::to_string(ntracks) + " of " +
                     std::to_string(p.lists_tab ? library.lists[p.open_list].tracks.size() : library.tracks.size()));
    s.text(s.w - 1 - utf8_len(count), y, count, ST_DIM);
    y++;
    for (auto& e : library.errors) { s.text(1, y++, fit(e, s.w - 2), S(C_BRED)); }

    // Missing lines of an open list go at the bottom, the way the web client
    // says so before the gig rather than after.
    std::vector<std::string> missing;
    if (p.lists_tab && p.open_list >= 0) missing = library.lists[p.open_list].missing;
    int footer = 1 + (missing.empty() ? 0 : 1);
    int view = s.h - y - footer;
    if (view < 1) return;
    if (p.cursor < p.scroll) p.scroll = p.cursor;
    if (p.cursor >= p.scroll + view) p.scroll = p.cursor - view + 1;
    if (p.scroll > std::max(0, (int)p.rows.size() - view)) p.scroll = std::max(0, (int)p.rows.size() - view);

    for (int i = 0; i < view && p.scroll + i < (int)p.rows.size(); i++) {
        const PickRow& r = p.rows[p.scroll + i];
        bool sel = p.scroll + i == p.cursor;
        int ry = y + i;
        if (sel) s.fill(0, ry, s.w, " ", ST_SELECT);
        if (r.list >= 0) {
            const TrackList& l = library.lists[r.list];
            std::string what = std::to_string(l.tracks.size()) + " tracks";
            if (!l.missing.empty()) what += ", " + std::to_string(l.missing.size()) + " missing";
            int lx = s.text(1, ry, "≡ ", sel ? ST_SELECT : ST_ACCENT);
            lx = s.text(lx, ry, fit(l.name, s.w / 2), sel ? ST_SELECT : ST_TITLE);
            s.text(lx + 2, ry, what, sel ? ST_SELECT : ST_DIM);
            std::string src = l.folder.empty() ? l.source : l.source + "/" + l.folder;
            s.text(s.w - 1 - utf8_len(fit(src, 30)), ry, fit(src, 30), sel ? ST_SELECT : ST_DIM);
            continue;
        }
        const Track& t = library.tracks[r.track];
        int indent = p.lists_tab && p.open_list < 0 ? 3 : 1;
        // Flags: one letter each, in a fixed column so they line up.
        const char* letters = "lcbo♪⇄";
        std::string fl;
        int li = 0;
        for (const char* q = letters; *q; li++) {
            int n = (unsigned char)*q >= 0xe0 ? 3 : 1;
            fl += flag(t.has, li) ? std::string(q, n) : "·";
            q += n;
        }
        s.text(indent, ry, fl, sel ? ST_SELECT : (t.chart.empty() ? ST_DIM : ST_ACCENT));
        Style ts = sel ? ST_SELECT : r.track == track ? S(C_BYELLOW, A_BOLD) : ST_TEXT;
        std::string folder = t.folder.empty() ? t.source : t.source + "/" + t.folder;
        int fw = std::min(utf8_len(folder), std::max(0, s.w / 3));
        int tx = indent + 7;
        s.text(tx, ry, fit(t.title, s.w - tx - fw - 3), ts);
        if (fw > 0) s.text(s.w - 1 - fw, ry, fit(folder, fw), sel ? ST_SELECT : ST_DIM);
    }
    if (p.rows.empty()) s.text(3, y, "nothing matches", ST_DIM);

    if (!missing.empty()) {
        std::string m = "not in the library: ";
        for (size_t i = 0; i < missing.size(); i++) m += (i ? ", " : "") + missing[i];
        s.text(1, s.h - 2, fit(m, s.w - 2), S(C_BRED));
    }
    struct { const char* k; const char* what; } keys[] = {
        {"↑↓", "move"}, {"enter", p.lists_tab && p.open_list < 0 ? "open" : "play"},
        {"esc", p.open_list >= 0 && p.lists_tab ? "back" : "close"}, {"^U", "clear"},
        {"lcbo♪⇄", "lyrics chords beats loops melody trade"},
    };
    x = 1;
    for (auto& k : keys) {
        int need = utf8_len(k.k) + 1 + (int)strlen(k.what) + 2;
        if (x + need > s.w - 1) break;
        x = s.text(x, s.h - 1, k.k, ST_KEY);
        x = s.text(x + 1, s.h - 1, k.what, ST_DIM) + 2;
    }
}

// --- keys --------------------------------------------------------------------

void App::key(int k) {
    if (k == ctrl('c') || k == ctrl('q')) { quit = true; return; }
    if (picker.open) picker_key(k);
    else play_key(k);
}

void App::play_key(int k) {
    switch (k) {
    case ' ': player.toggle(); break;
    case K_LEFT: player.nudge(-5); break;
    case K_RIGHT: player.nudge(5); break;
    case K_HOME: case 'H': player.seek(player.looping() ? player.programme[player.current].from : 0); break;
    case 'l': case 'L': case '/': open_picker(); break;
    case 'q': case 'Q': quit = true; break;
    case 'o': case 'O':
        if (player.looping()) { player.clear_loops(); say("loops cleared"); }
        break;
    case K_BACKSPACE: case K_DEL: player.remove_last_loop(); break;
    case '-': case '_': player.set_speed(player.speed - 0.05f); break;
    case '=': case '+': player.set_speed(player.speed + 0.05f); break;
    case '\\': player.set_speed(1.0f); break;
    case '[': player.transpose(-1, 0); break;
    case ']': player.transpose(1, 0); break;
    case ';': player.transpose(0, -1); break;
    case '\'': player.transpose(0, 1); break;
    case 'p': case 'P': player.reset_tuning(); break;
    case 'u': case 'U':
        // The record's A back on 440 with the cents; the semitones stay.
        if (chart.tuning != 440.0) {
            player.transpose(0, Player::retune_cents(chart.tuning) - player.cents);
            say("retuned A" + fmt_hz(chart.tuning) + " to A440");
        } else {
            say("this record is at A440");
        }
        break;
    default:
        if (k >= '1' && k <= '9') {
            size_t i = k - '1';
            if (i < chart.loops.size()) player.add_loop(chart.loops[i]);
        }
    }
}

void App::picker_key(int k) {
    Picker& p = picker;
    int n = (int)p.rows.size();
    auto move = [&](int to) {
        if (n == 0) return;
        p.cursor = std::max(0, std::min(n - 1, to));
        p.cursor_key = row_key(p.rows[p.cursor]);
    };
    for (int i = 0; i < N_FILTERS; i++)
        if (k == ctrl(FILTER_KEY[i])) { p.filters[i] = !p.filters[i]; refilter(); return; }
    switch (k) {
    case K_UP: move(p.cursor - 1); return;
    case K_DOWN: move(p.cursor + 1); return;
    case K_PGUP: move(p.cursor - 10); return;
    case K_PGDN: move(p.cursor + 10); return;
    case K_TAB: case K_SHIFT_TAB:
        p.lists_tab = !p.lists_tab;
        p.open_list = -1;
        refilter();
        return;
    case K_ESC:
        if (p.lists_tab && p.open_list >= 0) {
            p.cursor_key = "l" + std::to_string(p.open_list);
            p.open_list = -1;
            refilter();
        } else if (!p.query.empty()) {
            p.query.clear();
            refilter();
        } else if (track >= 0) {
            p.open = false;
        }
        return;
    case K_ENTER: {
        if (n == 0) return;
        const PickRow& r = p.rows[p.cursor];
        if (r.list >= 0) {
            p.open_list = r.list;
            p.query.clear();
            p.cursor_key.clear();
            refilter();
            return;
        }
        p.open = false;
        open_track(r.track, 0, true);
        return;
    }
    case K_BACKSPACE:
        if (!p.query.empty()) {
            p.query.resize(utf8_offset(p.query, utf8_len(p.query) - 1));
            refilter();
        }
        return;
    }
    if (k == ctrl('u')) { p.query.clear(); refilter(); return; }
    if (k >= 0x20 && k < 0x7f) {
        p.query += (char)k;
        refilter();
    } else if (k >= 0x80 && k < 0x10000 - 1 && k < K_UP) {
        // A non-ASCII letter, back to UTF-8.
        char buf[4];
        int len;
        if (k < 0x800) { buf[0] = 0xc0 | (k >> 6); buf[1] = 0x80 | (k & 0x3f); len = 2; }
        else { buf[0] = 0xe0 | (k >> 12); buf[1] = 0x80 | ((k >> 6) & 0x3f); buf[2] = 0x80 | (k & 0x3f); len = 3; }
        p.query.append(buf, len);
        refilter();
    }
}
