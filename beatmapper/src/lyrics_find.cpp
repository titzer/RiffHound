#include "lyrics_find.h"
#include "subprocess.h"
#include "platform.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// --- state --------------------------------------------------------------------

static std::thread              s_worker;
static std::atomic<bool>        s_running{false};
static std::atomic<bool>        s_done{false};
static std::atomic<pid_t>       s_pid{0};
static std::atomic<bool>        s_cancel{false};
static LyricsFindStatus         s_status = LF_IDLE;
static bool                     s_cached_only = false;
static std::vector<LyricsCand>  s_results;     // main thread
static std::vector<LyricsCand>  s_wresults;    // worker, swapped in on landing
static int                      s_wexit = 0;
static std::mutex               s_msg_mu;
static std::string              s_msg;
static std::string              s_msg_main;    // copy the main thread hands out

static void set_msg(const std::string& m) {
    std::lock_guard<std::mutex> g(s_msg_mu);
    s_msg = m;
}

// --- locating the script ----------------------------------------------------------

// The app may be launched through a symlink (~/bin/beatmapper) or from
// Finder with a minimal PATH, so look next to the executable's real path
// too, and for python3 in the usual prefixes.
static bool resolve_tool(std::vector<std::string>* argv) {
    const char* env = getenv("BM_LYRICS_CMD");
    if (env && env[0]) { argv->assign(1, env); return true; }
    std::string bases[2] = { "", "" };
    int nb = 1;
    char exe[4096];
    if (platform_exe_path(exe, sizeof(exe))) {
        char* slash = strrchr(exe, '/');
        if (slash) { slash[1] = 0; bases[nb++] = exe; }
    }
    for (int i = 0; i < nb; i++) {
        std::string sc = bases[i] + "scripts/lyrics_find.py";
        if (access(sc.c_str(), R_OK) != 0) continue;
        std::string py = bases[i] + "external/venv-lyrics/bin/python";
        if (access(py.c_str(), X_OK) != 0) {
            py = "python3";
            for (const char* d : { "/opt/homebrew/bin/python3", "/usr/local/bin/python3",
                                   "/usr/bin/python3" })
                if (access(d, X_OK) == 0) { py = d; break; }
        }
        *argv = { py, sc };
        return true;
    }
    return false;
}

bool lyrics_find_available() {
    std::vector<std::string> a;
    return resolve_tool(&a);
}

// --- parsing --------------------------------------------------------------------

static std::vector<std::string> split_tabs(const char* line) {
    std::vector<std::string> f;
    const char* p = line;
    for (;;) {
        const char* t = strchr(p, '\t');
        if (!t) {
            size_t n = strlen(p);
            while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r')) n--;
            f.emplace_back(p, n);
            break;
        }
        f.emplace_back(p, t - p);
        p = t + 1;
    }
    return f;
}

static void worker_main(std::vector<std::string> argv) {
    const char* home = getenv("HOME");
    std::string log;
    if (home) {
        std::string dir = std::string(home) + "/.cache";
        mkdir(dir.c_str(), 0755);
        dir += "/beatmapper"; mkdir(dir.c_str(), 0755);
        dir += "/lyrics";     mkdir(dir.c_str(), 0755);
        log = dir + "/log.txt";
    }
    Subprocess p;
    if (!subprocess_open(&p, argv, log.empty() ? nullptr : log.c_str())) {
        set_msg("Could not start the lyrics search");
        s_wexit = 1;
        s_done.store(true);
        return;
    }
    s_pid.store(p.pid);
    std::string last_warn;
    char line[8192];
    while (fgets(line, sizeof(line), p.out)) {
        std::vector<std::string> f = split_tabs(line);
        if (f.empty()) continue;
        if (f[0] == "status" && f.size() >= 2) set_msg(f[1]);
        else if (f[0] == "warn" && f.size() >= 2) last_warn = f[1];
        else if (f[0] == "cand" && f.size() >= 12) {
            LyricsCand c;
            c.rank       = atoi(f[1].c_str());
            c.score      = (float)atof(f[2].c_str());
            c.source_id  = f[3];
            c.kind       = f[4];
            c.artist     = f[5];
            c.title      = f[6];
            c.album      = f[7];
            c.duration   = f[8].empty() ? -1.0 : atof(f[8].c_str());
            c.n_lines    = atoi(f[9].c_str());
            c.timed_path = f[10];
            c.txt_path   = f[11];
            s_wresults.push_back(c);
        }
    }
    s_wexit = subprocess_close(&p);
    s_pid.store(0);
    if (s_wexit == 0)      set_msg(std::to_string(s_wresults.size()) + " found");
    else if (s_wexit == 2) set_msg("No lyrics found");
    else if (s_wexit == 3) set_msg("Not searched yet");
    else                   set_msg(last_warn.empty() ? "Lyrics search failed (see ~/.cache/beatmapper/lyrics/log.txt)"
                                                     : "Lyrics search failed: " + last_warn);
    s_done.store(true);
}

// --- public ---------------------------------------------------------------------

void lyrics_find_start(const char* audio_path, bool cached_only) {
    if (s_running.load() || !audio_path || !audio_path[0]) return;
    if (s_worker.joinable()) s_worker.join();
    std::vector<std::string> argv;
    if (!resolve_tool(&argv)) {
        s_status = LF_FAILED;
        set_msg("scripts/lyrics_find.py not found");
        return;
    }
    if (argv.size() == 1) argv = subprocess_shell_argv(argv[0].c_str(), audio_path);
    else                  argv.push_back(audio_path);
    if (cached_only) argv.push_back("--check");
    s_wresults.clear();
    s_wexit = 0;
    s_cached_only = cached_only;
    s_cancel.store(false);
    s_done.store(false);
    s_running.store(true);
    s_status = LF_RUNNING;
    set_msg(cached_only ? "Checking the lyrics cache" : "Searching for lyrics");
    s_worker = std::thread(worker_main, argv);
}

bool lyrics_find_poll() {
    {
        std::lock_guard<std::mutex> g(s_msg_mu);
        s_msg_main = s_msg;
    }
    if (!s_running.load() || !s_done.load()) return false;
    if (s_worker.joinable()) s_worker.join();
    s_running.store(false);
    if (s_cancel.load()) { s_status = LF_IDLE; return false; }
    s_results.swap(s_wresults);
    s_wresults.clear();
    s_status = (s_wexit == 0 || s_wexit == 2 || s_wexit == 3) ? LF_DONE : LF_FAILED;
    if (s_wexit == 3) s_status = LF_IDLE;          // nothing cached: as if never searched
    printf("[lyrics] search%s: exit %d, %d candidates\n", s_cached_only ? " (cache)" : "",
           s_wexit, (int)s_results.size());
    return true;
}

LyricsFindStatus lyrics_find_status()          { return s_status; }
bool             lyrics_find_was_cached_only() { return s_cached_only; }
const char*      lyrics_find_message()         { return s_msg_main.c_str(); }
const std::vector<LyricsCand>& lyrics_find_results() { return s_results; }

void lyrics_find_reset() {
    if (s_running.load()) {
        s_cancel.store(true);
        pid_t pid = s_pid.load();
        if (pid > 0) kill(pid, SIGTERM);
    }
    if (s_worker.joinable()) s_worker.join();
    s_running.store(false);
    s_done.store(false);
    s_results.clear();
    s_wresults.clear();
    s_status = LF_IDLE;
    set_msg("");
}

// --- files --------------------------------------------------------------------------

bool lyrics_read_lines(const std::string& path, std::vector<std::string>* out) {
    out->clear();
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) n--;
        if (n > 0) out->emplace_back(line, n);
    }
    fclose(f);
    return true;
}

bool lyrics_read_timed(const std::string& path, std::vector<LyricTimedLine>* out) {
    out->clear();
    if (path.empty()) return false;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::vector<std::string> fl = split_tabs(line);
        if (fl.size() < 3 || fl[0].empty() || fl[2].empty()) continue;
        LyricTimedLine t;
        t.t0 = atof(fl[0].c_str());
        t.t1 = fl[1].empty() ? -1.0 : atof(fl[1].c_str());
        t.text = fl[2];
        out->push_back(t);
    }
    fclose(f);
    return true;
}

static std::string dir_of(const std::string& p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

void lyrics_find_remember_choice(const LyricsCand& c) {
    std::string d = dir_of(c.txt_path);
    if (d.empty()) return;
    std::string tmp = d + "/chosen.tmp", fin = d + "/chosen";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f, "%s\n", c.source_id.c_str());
    fclose(f);
    rename(tmp.c_str(), fin.c_str());
}

std::string lyrics_find_remembered_choice() {
    if (s_results.empty()) return "";
    std::vector<std::string> l;
    if (!lyrics_read_lines(dir_of(s_results[0].txt_path) + "/chosen", &l) || l.empty()) return "";
    return l[0];
}
