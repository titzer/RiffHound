#pragma once

struct Lyric {
    double t_start;
    double t_end;
    char   text[128];
};

struct LyricMap {
    Lyric* lyrics;
    int    count;
    int    capacity;
    bool   dirty;
    int    selected_idx;  // index of selected lyric, or -1
};

void lyricmap_init    (LyricMap* lm);
void lyricmap_shutdown(LyricMap* lm);
void lyricmap_clear   (LyricMap* lm);

// Add a lyric sorted by t_start. Swaps t_start/t_end if needed.
// Returns insertion index, or -1 on allocation failure.
int  lyricmap_add(LyricMap* lm, double t_start, double t_end, const char* text);

// Remove lyric at index idx.
void lyricmap_remove(LyricMap* lm, int idx);
