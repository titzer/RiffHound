// karaoke: the RiffHound karaoke screen, in a terminal.
//
//   karaoke                          the folders in ~/.config/riffhound/karaoke.conf
//   karaoke ~/playalong ~/backing    these folders (and remember them)
//   karaoke ~/playalong/song.mp3     that song, playing; its folder is the library
//   karaoke -t 92 song.mp3           ...from 92 seconds in
//   karaoke --server URL             a library server (not yet: local only)
//
//   karaoke --dump 30,31.5 song.mp3  draw those moments as plain text and exit,
//                                    without audio (--size WxH, default 80x24)
//   karaoke --dump-picker QUERY DIR  the load dialog, filtered by QUERY
//   --keys STR                       with either: type STR first (\t \r \e
//                                    for tab, enter, escape; ^X for ctrl-X)

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"

static void usage() {
    fprintf(stderr,
            "usage: karaoke [-t SECONDS] [--server URL] [DIR|TRACK ...]\n"
            "       karaoke --dump T1,T2,... [--size WxH] TRACK\n"
            "       karaoke --dump-picker QUERY [--size WxH] DIR ...\n"
            "       (either dump takes --keys STR: keys to press first)\n");
}

static bool is_dir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static std::string parent_of(const std::string& p) {
    char buf[4096];
    std::string abs = realpath(p.c_str(), buf) ? buf : p;
    size_t s = abs.find_last_of('/');
    return s == std::string::npos ? "." : abs.substr(0, s);
}

int main(int argc, char** argv) {
    std::vector<std::string> paths, servers;
    std::string dump, dump_picker, keys_in;
    bool picker_dump = false;
    double start_at = 0;
    int dw = 80, dh = 24;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); exit(2); }
            return argv[++i];
        };
        if (a == "-t") start_at = atof(next().c_str());
        else if (a == "--server") servers.push_back(next());
        else if (a == "--dump") dump = next();
        else if (a == "--dump-picker") { dump_picker = next(); picker_dump = true; }
        else if (a == "--keys") keys_in = next();
        else if (a == "--size") {
            std::string v = next();
            if (sscanf(v.c_str(), "%dx%d", &dw, &dh) != 2) { usage(); return 2; }
        } else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') { usage(); return 2; }
        else paths.push_back(a);
    }

    // Folders are the library; a file names a track to open, and its folder
    // joins the library so the load dialog has its neighbours in it.
    std::vector<std::string> dirs, files;
    for (auto& p : paths) {
        if (is_dir(p)) dirs.push_back(p);
        else { files.push_back(p); dirs.push_back(parent_of(p)); }
    }
    Config cfg = config_load();
    bool headless = !dump.empty() || picker_dump;
    if (!headless && (!paths.empty() || !servers.empty())) {
        // Folders named on the command line become the defaults -- but a
        // single track's folder does not; that was a one-off.
        if (files.empty() && !dirs.empty()) cfg.dirs = dirs;
        if (!servers.empty()) cfg.servers = servers;
        config_save(cfg);
    }
    if (dirs.empty()) dirs = cfg.dirs;
    if (servers.empty()) servers = cfg.servers;

    App app;
    app.library.roots = dirs;
    app.library.scan();
    for (auto& s : servers)
        app.library.errors.push_back("server " + s + ": not supported yet; local folders only");
    if (dirs.empty() && servers.empty())
        app.library.errors.push_back("no folders: run karaoke DIR ... or add 'dir PATH' to " +
                                     config_dir() + "/karaoke.conf");

    int want = -1;
    if (!files.empty()) {
        want = app.library.find_media(files[0]);
        if (want < 0) { fprintf(stderr, "karaoke: not a track: %s\n", files[0].c_str()); return 1; }
    }

    // --- headless ------------------------------------------------------------
    if (headless) {
        app.audio = false;
        Screen s;
        s.resize(dw, dh);
        auto feed = [&]() {
            for (size_t i = 0; i < keys_in.size(); i++) {
                int k = (unsigned char)keys_in[i];
                if (k == '\\' && i + 1 < keys_in.size()) {
                    char e = keys_in[++i];
                    k = e == 't' ? K_TAB : e == 'r' ? K_ENTER : e == 'e' ? K_ESC
                      : e == 'd' ? K_DOWN : e == 'u' ? K_UP : e;
                } else if (k == '^' && i + 1 < keys_in.size()) {
                    k = ctrl(keys_in[++i]);
                }
                app.key(k);
            }
        };
        if (picker_dump) {
            app.picker.query = dump_picker;
            app.open_picker();
            feed();
            app.draw(s);
            fputs(s.plain().c_str(), stdout);
            return 0;
        }
        if (want < 0) { usage(); return 2; }
        app.open_track(want, 0, false);
        feed();
        for (const char* p = dump.c_str(); *p;) {
            char* end;
            double t = strtod(p, &end);
            if (end == p) break;
            app.fake_time = t;
            app.draw(s);
            printf("──── t=%.2f ────\n%s", t, s.plain().c_str());
            p = *end == ',' ? end + 1 : end;
        }
        return 0;
    }

    // --- interactive ---------------------------------------------------------
    if (!isatty(STDIN_FILENO)) { fprintf(stderr, "karaoke: needs a terminal\n"); return 1; }

    // The audio code prints as it goes; that goes to a log, not over the screen.
    std::string log = config_dir() + "/karaoke.log";
    int lfd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lfd >= 0) { dup2(lfd, STDOUT_FILENO); dup2(lfd, STDERR_FILENO); close(lfd); }
    setvbuf(stdout, nullptr, _IOLBF, 0);

    if (!term_open()) { fprintf(stderr, "karaoke: cannot open the terminal\n"); return 1; }
    app.player.init();

    Screen s;
    int w = 80, h = 24;
    term_size(&w, &h);
    s.resize(w, h);

    auto frame = [&](bool full) {
        app.draw(s);
        term_write(s.diff(full));
    };

    if (want >= 0) {
        s.clear();
        s.text(2, 1, "loading " + app.library.tracks[want].title + " …", S(C_GRAY));
        term_write(s.diff(true));
        app.open_track(want, start_at, true);
    } else {
        app.open_picker();
    }

    bool full = true;
    std::vector<int> keys;
    while (!app.quit) {
        if (term_resized() && term_size(&w, &h)) { s.resize(w, h); full = true; }
        keys.clear();
        term_read_keys(16, &keys);
        for (int k : keys) {
            bool was_open = app.picker.open;
            int before = app.track;
            // Opening a track decodes it first, which takes a moment; say so.
            if (was_open && k == K_ENTER && !app.picker.rows.empty() &&
                app.picker.rows[app.picker.cursor].track >= 0) {
                s.clear();
                s.text(2, 1, "loading " + app.library.tracks[app.picker.rows[app.picker.cursor].track].title + " …",
                       S(C_GRAY));
                term_write(s.diff(false));
            }
            app.key(k);
            if (app.track != before) full = true;
        }
        app.player.update();
        frame(full);
        full = false;
    }

    term_close();
    app.player.shutdown();
    return 0;
}
