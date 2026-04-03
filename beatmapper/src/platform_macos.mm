#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "platform.h"
#include <string.h>

bool platform_open_file_dialog(char* out_path, int out_size) {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories    = NO;
    panel.canChooseFiles          = YES;
    panel.allowedContentTypes = @[
        [UTType typeWithFilenameExtension:@"mp3"],
        [UTType typeWithFilenameExtension:@"wav"],
        [UTType typeWithFilenameExtension:@"flac"],
        [UTType typeWithFilenameExtension:@"m4a"],
    ];

    if ([panel runModal] == NSModalResponseOK) {
        const char* utf8 = [panel.URL.path UTF8String];
        strncpy(out_path, utf8, out_size - 1);
        out_path[out_size - 1] = '\0';
        return true;
    }
    return false;
}

bool platform_open_beatmap_dialog(char* out_path, int out_size) {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories    = NO;
    panel.canChooseFiles          = YES;
    panel.allowedContentTypes = @[
        [UTType typeWithFilenameExtension:@"txt"],
    ];

    if ([panel runModal] == NSModalResponseOK) {
        const char* utf8 = [panel.URL.path UTF8String];
        strncpy(out_path, utf8, out_size - 1);
        out_path[out_size - 1] = '\0';
        return true;
    }
    return false;
}

bool platform_save_beatmap_dialog(char* out_path, int out_size,
                                  const char* suggested_name) {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.allowedContentTypes = @[
        [UTType typeWithFilenameExtension:@"txt"],
    ];
    if (suggested_name && suggested_name[0])
        panel.nameFieldStringValue = [NSString stringWithUTF8String:suggested_name];

    if ([panel runModal] == NSModalResponseOK) {
        const char* utf8 = [panel.URL.path UTF8String];
        strncpy(out_path, utf8, out_size - 1);
        out_path[out_size - 1] = '\0';
        return true;
    }
    return false;
}
