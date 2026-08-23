#pragma once

// ---------------------------------------------------------------------------
// The track library: folders the user has opened from, indexed.
//
// Like the karaoke app's library, the Open dialog is a search box over every
// audio file in the remembered folders, each tagged with what its companion
// .txt carries (beats, sections, chords, lyrics, loops), so "the ones I have
// not chorded yet" is a filter rather than a memory exercise.  Folders are
// remembered in ~/.beatmapper_dirs; the index is rebuilt from the file system
// at startup and whenever a folder is added, and a sidecar is re-read only
// when its size or mtime changed.
// ---------------------------------------------------------------------------

static const int LIB_MAX_DIRS = 16;

struct LibFlags {
    bool beats, sections, chords, lyrics, loops;
};

struct LibEntry {
    char     path[512];      // audio file
    char     title[128];     // file name without extension
    char     dir[512];       // folder it was found in
    bool     has_txt;        // companion .txt exists
    LibFlags has;            // what the .txt carries (all false without one)
    int      n_beats;        // beats in the .txt, for the listing
    long     txt_size;       // sidecar stamp
    long     txt_mtime;
    double   last_opened;    // seconds since epoch from the recent list, 0 = never
};

struct Library {
    char      dirs[LIB_MAX_DIRS][512];
    int       dir_count;
    LibEntry* entries;
    int       count, capacity;
};

void library_init(Library* lib);
void library_shutdown(Library* lib);

// Read the remembered folders from ~/.beatmapper_dirs and index them.
void library_load(Library* lib);
void library_save(const Library* lib);

// Remember a folder (most recent first, deduplicated) and index it.  Adding
// the folder of a file picked with the system dialog is how the library
// grows without a separate step.
void library_add_dir(Library* lib, const char* dir);
void library_remove_dir(Library* lib, int idx);

// Re-index every remembered folder (cheap when nothing changed).
void library_rescan(Library* lib);

// Mark an entry as just opened (for the recency sort).
void library_touch(Library* lib, const char* path);

// Re-read one entry's sidecar (after a save).
void library_refresh(Library* lib, const char* path);

// Subsequence match, case-insensitive: every letter of q in order in s.
bool library_fuzzy(const char* q, const char* s);

// What a sidecar carries, read from the file.  Exposed for the listing.
bool library_read_flags(const char* txt_path, LibFlags* out, int* n_beats);
