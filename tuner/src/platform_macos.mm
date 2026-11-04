#import <Cocoa/Cocoa.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdlib.h>
#include "platform.h"

void platform_toggle_fullscreen(GLFWwindow* window) {
    NSWindow* win = glfwGetCocoaWindow(window);
    if (!win) return;
    // A floating (always-on-top) window cannot enter a fullscreen Space.
    if (!(win.styleMask & NSWindowStyleMaskFullScreen)) [win setLevel:NSNormalWindowLevel];
    win.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    [win toggleFullScreen:nil];
}

bool platform_is_fullscreen(GLFWwindow* window) {
    NSWindow* win = glfwGetCocoaWindow(window);
    return win && (win.styleMask & NSWindowStyleMaskFullScreen);
}

void platform_set_app_icon() {
    char raw[PATH_MAX];
    uint32_t n = sizeof(raw);
    if (_NSGetExecutablePath(raw, &n) != 0) return;
    char real[PATH_MAX];
    if (!realpath(raw, real)) return;
    NSString* exe  = [NSString stringWithUTF8String:real];
    NSString* icns = [[[exe stringByDeletingLastPathComponent] stringByDeletingLastPathComponent]
                         stringByAppendingPathComponent:@"Resources/Tuner.icns"];
    NSImage* img = [[NSImage alloc] initWithContentsOfFile:icns];
    if (img) [NSApp setApplicationIconImage:img];
}
