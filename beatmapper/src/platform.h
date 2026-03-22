#pragma once

// Platform-specific helpers: system file dialog, etc.

// Show the OS native "open file" dialog filtered to audio files.
// Fills out_path and returns true if the user selected a file.
// Returns false if the user cancelled.
bool platform_open_file_dialog(char* out_path, int out_size);
