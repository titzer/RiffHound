#include "library.h"

#include <algorithm>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// Ordered: two files with the same stem are one track, and the earlier
// extension wins (server.py MEDIA_EXT).
static const char* MEDIA_EXT[] = {".mp3", ".m4a", ".ogg", ".webm", ".flac", ".wav",
                                  ".mp4", ".m4v", ".mov"};
static const int N_MEDIA_EXT = sizeof MEDIA_EXT / sizeof MEDIA_EXT[0];

static std::string lower(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

static int media_rank(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return -1;
    std::string ext = lower(name.substr(dot));
    for (int i = 0; i < N_MEDIA_EXT; i++)
        if (ext == MEDIA_EXT[i]) return i;
    return -1;
}

static bool is_file(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_dir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static std::string expand(const std::string& p) {
    std::string q = p;
    if (!q.empty() && q[0] == '~') {
        const char* home = getenv("HOME");
        q = std::string(home ? home : "") + q.substr(1);
    }
    char buf[PATH_MAX];
    if (realpath(q.c_str(), buf)) return buf;
    return q;
}

static std::string base_name(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

static std::string strip_ext(const std::string& p) {
    size_t dot = p.rfind('.'), slash = p.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return p;
    return p.substr(0, dot);
}

// "Battle_Hymn" and "battle hymn" are the same song (server.py list_key).
static std::string list_key(const std::string& s) {
    std::string o;
    bool gap = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '_' || c == '-') { gap = true; continue; }
        if (gap && !o.empty()) o += ' ';
        gap = false;
        o += (char)tolower((unsigned char)c);
    }
    return o;
}

static std::string title_of(const std::string& stem) {
    std::string o;
    bool gap = false;
    for (char c : stem) {
        if (c == '_' || c == '-') { gap = true; continue; }
        if (gap) o += ' ';
        gap = false;
        o += c;
    }
    size_t a = o.find_first_not_of(' '), b = o.find_last_not_of(' ');
    return a == std::string::npos ? "" : o.substr(a, b - a + 1);
}

struct Walk {
    int index;
    std::string root;
    std::map<std::string, int>* by_id;
    std::vector<Track>* tracks;
    std::vector<std::pair<std::string, std::string>>* list_files;  // root, path
};

static void walk(Walk& w, const std::string& dir, const std::string& rel) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> files, dirs;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        std::string p = dir + "/" + n;
        if (is_dir(p)) dirs.push_back(n);
        else files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    std::sort(dirs.begin(), dirs.end());

    for (auto& n : files) {
        std::string path = dir + "/" + n;
        std::string ln = lower(n);
        if (ln.size() > 9 && ln.compare(0, 5, "list-") == 0 && ln.compare(ln.size() - 4, 4, ".txt") == 0) {
            w.list_files->push_back({w.root, path});
            continue;
        }
        int rank = media_rank(n);
        if (rank < 0) continue;
        std::string relpath = rel.empty() ? n : rel + "/" + n;
        std::string id = std::to_string(w.index) + "/" + strip_ext(relpath);
        auto it = w.by_id->find(id);
        if (it != w.by_id->end()) {
            Track& old = (*w.tracks)[it->second];
            if (media_rank(old.media) <= rank) continue;
        }
        Track t;
        t.id = id;
        t.title = title_of(strip_ext(n));
        t.source = base_name(w.root);
        t.folder = rel;
        t.media = path;
        std::string chart = strip_ext(path) + ".txt";
        if (is_file(chart)) {
            t.chart = chart;
            std::string text;
            if (read_file(chart, &text)) t.has = chart_flags(text);
        }
        if (it != w.by_id->end()) (*w.tracks)[it->second] = t;
        else { (*w.by_id)[id] = (int)w.tracks->size(); w.tracks->push_back(t); }
    }
    for (auto& n : dirs) {
        std::string p = dir + "/" + n;
        if (is_file(p + "/.riffignore")) continue;
        walk(w, p, rel.empty() ? n : rel + "/" + n);
    }
}

void Library::scan() {
    tracks.clear();
    lists.clear();
    errors.clear();
    std::map<std::string, int> by_id;
    std::vector<std::pair<std::string, std::string>> list_files;

    // Distinct roots, none inside another: a subfolder of a root is already
    // covered, and would otherwise list everything in it twice.
    std::vector<std::string> rs;
    for (auto& r : roots) {
        std::string e = expand(r);
        if (!is_dir(e)) { errors.push_back("not a folder: " + r); continue; }
        rs.push_back(e);
    }
    std::sort(rs.begin(), rs.end(), [](const std::string& a, const std::string& b) { return a.size() < b.size(); });
    std::vector<std::string> kept;
    for (auto& r : rs) {
        bool inside = false;
        for (auto& k : kept)
            if (r == k || (r.size() > k.size() && r.compare(0, k.size(), k) == 0 && r[k.size()] == '/'))
                inside = true;
        if (!inside) kept.push_back(r);
    }
    roots = kept;

    for (size_t i = 0; i < roots.size(); i++) {
        Walk w = {(int)i, roots[i], &by_id, &tracks, &list_files};
        walk(w, roots[i], "");
    }

    // Lists, resolved now that every track is known.
    std::map<std::string, std::vector<int>> keys;
    for (size_t i = 0; i < tracks.size(); i++) {
        const Track& t = tracks[i];
        std::string rel = t.id.substr(t.id.find('/') + 1);
        std::string stem = base_name(rel);
        std::string ext = t.media.substr(strip_ext(t.media).size());
        std::vector<std::string> ks = {list_key(t.title), list_key(stem), list_key(stem + ext),
                                       list_key(rel), list_key(rel + ext)};
        std::sort(ks.begin(), ks.end());
        ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
        for (auto& k : ks) keys[k].push_back((int)i);
    }
    for (auto& lf : list_files) {
        TrackList l;
        std::string fname = base_name(lf.second);
        l.name = fname.substr(5, fname.size() - 9);
        l.source = base_name(lf.first);
        std::string dir = lf.second.substr(0, lf.second.size() - fname.size() - 1);
        l.folder = dir.size() > lf.first.size() ? dir.substr(lf.first.size() + 1) : "";
        std::string text;
        read_file(lf.second, &text);
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? text.size() : nl + 1;
            size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
            if (a == std::string::npos) continue;
            line = line.substr(a, b - a + 1);
            if (line[0] == '#') continue;
            auto it = keys.find(list_key(line));
            if (it == keys.end()) l.missing.push_back(line);
            else for (int ti : it->second) l.tracks.push_back(ti);
        }
        lists.push_back(l);
    }

    auto rec = recents_load();
    for (auto& t : tracks) {
        auto it = rec.find(t.media);
        if (it != rec.end()) t.last = it->second;
    }
}

int Library::find_media(const std::string& abs_path) const {
    std::string p = expand(abs_path);
    for (size_t i = 0; i < tracks.size(); i++)
        if (tracks[i].media == p) return (int)i;
    // Same stem, another extension: the track the walk preferred.
    std::string stem = strip_ext(p);
    for (size_t i = 0; i < tracks.size(); i++)
        if (strip_ext(tracks[i].media) == stem) return (int)i;
    return -1;
}

int fuzzy_score(const std::string& q, const std::string& s) {
    // The best alignment of the query's letters in order, by dynamic
    // programming: greedy leftmost matching would score "bdog" against the
    // B, D, o, g of "Be Sure to Drink Your Ovaltine (Grunge)" and miss Black
    // Dog.  A letter scores 1, 8 more at the start of a word, 6 more right
    // after the previous letter; each skipped character costs 1.  Spaces in
    // the query separate words and match anything.
    std::string lq;
    for (char c : q) if (c != ' ') lq += (char)tolower((unsigned char)c);
    if (lq.empty()) return 0;
    std::string ls = lower(s);
    int m = (int)lq.size(), n = (int)ls.size();
    if (m > n) return -1;
    const int NONE = -1000000;
    std::vector<int> prev(n, NONE), cur(n, NONE);
    for (int i = 0; i < m; i++) {
        int run = NONE;  // max over k < j-1 of prev[k] + k
        for (int j = 0; j < n; j++) {
            cur[j] = NONE;
            if (ls[j] == lq[i]) {
                bool word = j == 0 || !isalnum((unsigned char)ls[j - 1]);
                int bonus = 1 + (word ? 8 : 0);
                if (i == 0) {
                    cur[j] = bonus - j / 4;
                } else {
                    int best = NONE;
                    if (j > 0 && prev[j - 1] > NONE) best = prev[j - 1] + 6;
                    if (run > NONE) best = std::max(best, run - j + 1);
                    if (best > NONE) cur[j] = best + bonus;
                }
            }
            if (i > 0 && j > 0 && prev[j - 1] > NONE) run = std::max(run, prev[j - 1] + (j - 1));
        }
        std::swap(prev, cur);
    }
    int best = NONE;
    for (int v : prev) best = std::max(best, v);
    if (best <= NONE) return -1;
    // A shorter title that matches as well is the likelier one.
    return std::max(0, best * 4 - n / 8 + 400);
}

// --- config -----------------------------------------------------------------

std::string config_dir() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    std::string base = xdg && *xdg ? xdg : std::string(home ? home : ".") + "/.config";
    mkdir(base.c_str(), 0755);
    std::string d = base + "/riffhound";
    mkdir(d.c_str(), 0755);
    return d;
}

Config config_load() {
    Config c;
    std::string text;
    if (!read_file(config_dir() + "/karaoke.conf", &text)) return c;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line[a] == '#') continue;
        size_t sp = line.find_first_of(" \t", a);
        if (sp == std::string::npos) continue;
        std::string k = line.substr(a, sp - a);
        size_t v0 = line.find_first_not_of(" \t", sp), v1 = line.find_last_not_of(" \t\r");
        if (v0 == std::string::npos) continue;
        std::string v = line.substr(v0, v1 - v0 + 1);
        if (k == "dir") c.dirs.push_back(v);
        else if (k == "server") c.servers.push_back(v);
    }
    return c;
}

bool config_save(const Config& c) {
    FILE* f = fopen((config_dir() + "/karaoke.conf").c_str(), "w");
    if (!f) return false;
    fprintf(f, "# RiffHound terminal karaoke.  One per line:\n"
               "#   dir PATH       a local folder of tracks\n"
               "#   server URL     a library server (webui/karaoke/server.py)\n");
    for (auto& d : c.dirs) fprintf(f, "dir %s\n", d.c_str());
    for (auto& s : c.servers) fprintf(f, "server %s\n", s.c_str());
    fclose(f);
    return true;
}

std::map<std::string, double> recents_load() {
    std::map<std::string, double> m;
    std::string text;
    if (!read_file(config_dir() + "/karaoke-recent.tsv", &text)) return m;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        m[line.substr(tab + 1)] = atof(line.substr(0, tab).c_str());
    }
    return m;
}

void recents_touch(const std::string& key) {
    auto m = recents_load();
    m[key] = (double)time(nullptr);
    FILE* f = fopen((config_dir() + "/karaoke-recent.tsv").c_str(), "w");
    if (!f) return;
    for (auto& kv : m) fprintf(f, "%.0f\t%s\n", kv.second, kv.first.c_str());
    fclose(f);
}
