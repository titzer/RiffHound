#pragma once

static const int RECENT_MAX = 5;

struct RecentFiles {
    char paths[RECENT_MAX][512];
    int  count;
};

void recent_init(RecentFiles* rf);
// Load from ~/.beatmapper (silently ignores a missing file).
void recent_load(RecentFiles* rf);
// Persist to ~/.beatmapper.
void recent_save(const RecentFiles* rf);
// Prepend path, deduplicate, trim to RECENT_MAX.
void recent_add(RecentFiles* rf, const char* path);
