#pragma once

// Platform-specific helpers: system file dialog, etc.

struct GLFWwindow;

// Show the OS native "open file" dialog filtered to audio files.
// Fills out_path and returns true if the user selected a file.
// Returns false if the user cancelled.
bool platform_open_file_dialog(char* out_path, int out_size);

// Show the OS native "choose folder" dialog.
bool platform_open_folder_dialog(char* out_path, int out_size);

// Show the OS native "open file" dialog filtered to .txt files (beatmap/timeseries).
bool platform_open_beatmap_dialog(char* out_path, int out_size);

// Register Cmd+F as the native macOS fullscreen shortcut.
// Must be called once after the window is created.
void platform_install_fullscreen_shortcut();

// The running executable's path, for re-launching it.  Returns false when
// the OS will not say, in which case argv[0] is the best that can be done.
bool platform_exe_path(char* out_path, int out_size);

// Show the OS native "save file" dialog for .txt files.
// suggested_name: default filename shown in the dialog (basename only, no path).
bool platform_save_beatmap_dialog(char* out_path, int out_size,
                                  const char* suggested_name);

// Give the process its icon (Dock on macOS, window/taskbar elsewhere) from
// straight RGBA pixels, row-major, top row first.  Call once after the
// window is created.
void platform_set_app_icon(GLFWwindow* window, const unsigned char* rgba, int w, int h);
