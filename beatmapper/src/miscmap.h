#pragma once

struct MiscAnnotation {
    double t_start;
    double t_end;
    char   text[128];  // everything after t_start and t_end on the original line
};

struct MiscMap {
    MiscAnnotation* entries;
    int             count;
    int             capacity;
    bool            dirty;
    int             selected_idx;
};

void miscmap_init    (MiscMap* mm);
void miscmap_shutdown(MiscMap* mm);
void miscmap_clear   (MiscMap* mm);

// Add an entry sorted by t_start. Returns insertion index, or -1 on failure.
int  miscmap_add   (MiscMap* mm, double t_start, double t_end, const char* text);

// Remove entry at index idx.
void miscmap_remove(MiscMap* mm, int idx);
