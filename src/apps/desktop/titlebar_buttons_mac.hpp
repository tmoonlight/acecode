#pragma once

#ifdef __APPLE__
#import <AppKit/AppKit.h>

namespace acecode::desktop::detail {
// Install once per window and reapply after the host configures its chrome.
void configure_mac_titlebar_buttons(NSWindow* window);
}
#endif
