#include "platform.h"
#include <stdio.h>
#include <string.h>

// Use zenity if available; otherwise stub out.
bool platform_open_file_dialog(char* out_path, int out_size) {
    FILE* f = popen(
        "zenity --file-selection --file-filter='Audio files (mp3 wav flac) | *.mp3 *.wav *.flac' 2>/dev/null",
        "r");
    if (!f) return false;
    char buf[1024] = {};
    if (!fgets(buf, sizeof(buf), f)) { pclose(f); return false; }
    pclose(f);
    // Strip trailing newline
    int len = (int)strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    if (buf[0] == '\0') return false;
    strncpy(out_path, buf, out_size - 1);
    out_path[out_size - 1] = '\0';
    return true;
}
