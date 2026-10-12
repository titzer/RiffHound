#pragma once
// The library: tracks and lists, from local folders today and from a library
// server (webui/karaoke/server.py) later.  Local scanning follows the server's
// rules exactly -- media extensions and their preference order, .riffignore,
// hidden files, list-<Name>.txt resolution -- so a folder looks the same
// whether it is opened here or served to the web client.

#include <map>
#include <string>
#include <vector>

#include "chart.h"

struct Track {
    std::string id;         // "<root index>/<path under the root, no extension>"
    std::string title;      // the stem, with _ and - as spaces
    std::string source;     // the root's folder name
    std::string folder;     // the folder under the root, "" at the top
    std::string media;      // absolute path (local) or URL (server)
    std::string chart;      // likewise, "" if there is none
    ChartFlags  has;
    double      last = 0;   // when it was last opened, epoch seconds; 0 never
};

struct TrackList {
    std::string name, source, folder;
    std::vector<int> tracks;            // indices into Library::tracks, in order
    std::vector<std::string> missing;   // lines that name nothing
};

struct Library {
    std::vector<std::string> roots;
    std::vector<Track>       tracks;
    std::vector<TrackList>   lists;
    std::vector<std::string> errors;

    void scan();                        // (re)walks every local root
    int  find_media(const std::string& abs_path) const;
};

// Every letter of the query, in order, case-insensitively -- the web client's
// match.  The score ranks matches: contiguous runs and word starts count more,
// so "bdog" puts Black Dog above Bad Moon Rising On A Dog.  -1 is no match.
int fuzzy_score(const std::string& q, const std::string& s);

// ~/.config/riffhound, created on demand.
std::string config_dir();

// karaoke.conf: "dir PATH" and "server URL" lines; # comments.
struct Config {
    std::vector<std::string> dirs, servers;
};
Config config_load();
bool   config_save(const Config& c);

// When each track was last opened, keyed by media path or URL.
std::map<std::string, double> recents_load();
void recents_touch(const std::string& key);
