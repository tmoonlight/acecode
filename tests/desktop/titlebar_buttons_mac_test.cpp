#ifdef __APPLE__
#include "desktop/titlebar_buttons_mac.hpp"

#include <gtest/gtest.h>
#include <array>

namespace {

constexpr std::array<NSWindowButton, 3> kButtons = {
    NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton,
};

NSRect button_rect(NSWindow* window, NSWindowButton type) {
    NSButton* button = [window standardWindowButton:type];
    return [button convertRect:button.bounds toView:nil];
}

TEST(MacTitlebarButtons, RestoresStartupCentersAfterWindowLayoutChanges) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        if (![NSScreen mainScreen]) GTEST_SKIP() << "Requires a macOS window server";
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 90, 720, 540)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                      NSWindowStyleMaskFullSizeContentView
              backing:NSBackingStoreBuffered defer:NO];
        window.titlebarAppearsTransparent = YES;
        window.titleVisibility = NSWindowTitleHidden;
        [window layoutIfNeeded];

        std::array<CGFloat, 3> native_offsets{};
        const NSRect close = button_rect(window, NSWindowCloseButton);
        for (std::size_t i = 0; i < kButtons.size(); ++i) {
            const NSRect rect = button_rect(window, kButtons[i]);
            ASSERT_FALSE(NSIsEmptyRect(rect));
            native_offsets[i] = NSMidX(rect) - NSMidX(close);
        }
        const auto expect_aligned = [&] {
            const NSRect red = button_rect(window, NSWindowCloseButton);
            EXPECT_NEAR(NSMinX(red), NSHeight(window.frame) - NSMaxY(red), 0.01);
            for (std::size_t i = 0; i < kButtons.size(); ++i) {
                const NSRect rect = button_rect(window, kButtons[i]);
                EXPECT_NEAR(NSHeight(window.frame) - NSMidY(rect), 20, 0.01);
                EXPECT_NEAR(NSMidX(rect) - NSMidX(red), native_offsets[i], 0.01);
            }
        };
        acecode::desktop::detail::configure_mac_titlebar_buttons(window);
        expect_aligned();
        NSButton* green = [window standardWindowButton:NSWindowZoomButton];
        [green setFrameOrigin:NSMakePoint(NSMinX(green.frame) + 3, NSMinY(green.frame))];
        acecode::desktop::detail::configure_mac_titlebar_buttons(window);
        expect_aligned();
        [window setContentSize:NSMakeSize(980, 640)];
        [window update];
        expect_aligned();
        [window zoom:nil];
        [window update];
        expect_aligned();
        [window zoom:nil];
        [window update];
        expect_aligned();

        for (NSNotificationName name in @[NSWindowDidResizeNotification,
                                          NSWindowDidDeminiaturizeNotification,
                                          NSWindowDidExitFullScreenNotification,
                                          NSWindowDidUpdateNotification]) {
            // Reproduce a later AppKit pass moving only the yellow button.
            NSButton* yellow = [window standardWindowButton:NSWindowMiniaturizeButton];
            [yellow setFrameOrigin:NSMakePoint(NSMinX(yellow.frame) + 4, NSMinY(yellow.frame) + 6)];
            [[NSNotificationCenter defaultCenter] postNotificationName:name object:window];
            expect_aligned();
            [[NSNotificationCenter defaultCenter] postNotificationName:name object:window];
            expect_aligned();
        }
    }
}

} // namespace
#endif
