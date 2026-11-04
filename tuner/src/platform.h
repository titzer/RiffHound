#pragma once

struct GLFWwindow;

// Enter/leave fullscreen (native Spaces fullscreen on macOS).
void platform_toggle_fullscreen(GLFWwindow* window);
bool platform_is_fullscreen(GLFWwindow* window);

// Use Contents/Resources/Tuner.icns as the Dock icon when the binary runs
// from inside the app bundle (also when launched through a symlink).
void platform_set_app_icon();
