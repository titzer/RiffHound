#pragma once

// Section kinds from format-spec.md — order matches the spec keyword list.
enum SectionKind : int {
    SK_INTRO = 0, SK_VERSE, SK_PRE_CHORUS, SK_CHORUS,
    SK_POST_CHORUS, SK_BRIDGE, SK_BREAKDOWN, SK_INSTRUMENTAL,
    SK_SOLO, SK_INTERLUDE, SK_OUTRO, SK_REFRAIN,
    SK_COUNT
};

// Canonical keyword strings (match format-spec.md exactly).
extern const char* const SECTION_KIND_NAMES[SK_COUNT];

struct Section {
    double      t_start;
    double      t_end;
    SectionKind kind;
    char        label[48];  // optional suffix after ':' (e.g. "1" in "verse: 1")
};

struct SectionMap {
    Section* sections;
    int      count;
    int      capacity;
    bool     dirty;
    int      selected_idx;  // index of selected section, or -1
};

void sectionmap_init    (SectionMap* sm);
void sectionmap_shutdown(SectionMap* sm);
void sectionmap_clear   (SectionMap* sm);

// Add a section sorted by t_start. Swaps t_start/t_end if needed.
// Returns insertion index, or -1 on allocation failure.
int  sectionmap_add(SectionMap* sm, double t_start, double t_end,
                    SectionKind kind, const char* label);

// Remove section at index idx.
void sectionmap_remove(SectionMap* sm, int idx);
