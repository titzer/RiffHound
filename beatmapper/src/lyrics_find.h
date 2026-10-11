#pragma once

#include <string>
#include <vector>

// Lyrics lookup: scripts/lyrics_find.py (stdlib Python, LRCLIB + NetEase)
// run on a worker thread for the loaded track.  It prints ranked candidates
// whose texts it writes to its cache (~/.cache/beatmapper/lyrics/<hash>/);
// synced ones also come with line timings.  One search at a time; a track
// change kills it.

struct LyricsCand {
    int         rank = 0;
    float       score = 0;
    std::string source_id;     // "lrclib:19080"
    std::string kind;          // synced | plain | instrumental
    std::string artist, title, album;
    double      duration = -1; // seconds, -1 unknown
    int         n_lines = 0;
    std::string timed_path;    // TSV start<TAB>end<TAB>text, empty unless synced
    std::string txt_path;      // one cleaned lyric line per line
};

struct LyricTimedLine {
    double      t0;
    double      t1;            // -1 when the source gives only starts
    std::string text;
};

enum LyricsFindStatus {
    LF_IDLE = 0,
    LF_RUNNING,
    LF_DONE,       // candidates in lyrics_find_results() (maybe none)
    LF_FAILED,     // lyrics_find_message() says why
};

bool             lyrics_find_available();   // python3 + the script found
// Search for `audio_path`'s lyrics.  cached_only: answer from the cache
// without the network (used on track load).  No-op while one runs.
void             lyrics_find_start(const char* audio_path, bool cached_only);
// Once per frame on the main thread: land a finished search.  Returns true
// on the frame results landed.
bool             lyrics_find_poll();
LyricsFindStatus lyrics_find_status();
bool             lyrics_find_was_cached_only();   // the last search was a cache check
const char*      lyrics_find_message();
const std::vector<LyricsCand>& lyrics_find_results();
// New track: stop a running search and forget the results.
void             lyrics_find_reset();

bool lyrics_read_lines(const std::string& path, std::vector<std::string>* out);
bool lyrics_read_timed(const std::string& path, std::vector<LyricTimedLine>* out);

// The candidate the user took, remembered in its cache dir so the timings
// come back with the track.  "" when none.
void        lyrics_find_remember_choice(const LyricsCand& c);
std::string lyrics_find_remembered_choice();   // source_id, from the last results' dir
