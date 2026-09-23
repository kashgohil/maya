#include "maya/platform/app_icon.hpp"

#include <iostream>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#import <AppKit/NSApplication.h>
#import <AppKit/NSImage.h>
#import <AppKit/NSWindow.h>
#pragma clang diagnostic pop

extern unsigned char maya_icon_png[];
extern unsigned int maya_icon_png_len;

namespace maya {

void set_application_icon(void* cocoa_window) {
    @autoreleasepool {
        static NSImage* image = nil;
        static bool loaded = false;
        if (!loaded) {
            loaded = true;
            NSData* data = [NSData dataWithBytes:maya_icon_png length:maya_icon_png_len];
            image = [[NSImage alloc] initWithData:data];
            if (image == nil) {
                std::cerr << "[Application] could not decode the application icon\n";
            } else {
                [[NSApplication sharedApplication] setApplicationIconImage:image];
            }
        }
        if (image != nil && cocoa_window != nullptr) {
            NSWindow* window = (__bridge NSWindow*)cocoa_window;
            [window setMiniwindowImage:image];
        }
    }
}

} // namespace maya
