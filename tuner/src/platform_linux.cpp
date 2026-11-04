// Untested fallback: GLFW monitor fullscreen, no app icon.
#include <GLFW/glfw3.h>
#include "platform.h"

static int s_saved[4];

void platform_toggle_fullscreen(GLFWwindow* window) {
    if (glfwGetWindowMonitor(window)) {
        glfwSetWindowMonitor(window, nullptr, s_saved[0], s_saved[1], s_saved[2], s_saved[3], 0);
        return;
    }
    glfwGetWindowPos(window, &s_saved[0], &s_saved[1]);
    glfwGetWindowSize(window, &s_saved[2], &s_saved[3]);
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(mon);
    glfwSetWindowMonitor(window, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
}

bool platform_is_fullscreen(GLFWwindow* window) { return glfwGetWindowMonitor(window) != nullptr; }
void platform_set_app_icon() {}
