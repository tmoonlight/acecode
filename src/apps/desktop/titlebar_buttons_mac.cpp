#include "titlebar_buttons_mac.hpp"

#ifdef __APPLE__
#import <objc/runtime.h>
#include <array>

@interface ACEWindowButtonLayout : NSObject
- (instancetype)initWithWindow:(NSWindow*)window;
- (void)alignInWindow:(NSWindow*)window;
@end

@implementation ACEWindowButtonLayout {
    std::array<CGFloat, 3> _centerOffsets;
    BOOL _hasOffsets;
    BOOL _aligning;
}

- (instancetype)initWithWindow:(NSWindow*)window {
    self = [super init];
    if (self) {
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        for (NSNotificationName name in @[NSWindowDidResizeNotification,
                                          NSWindowDidDeminiaturizeNotification,
                                          NSWindowDidExitFullScreenNotification,
                                          NSWindowDidUpdateNotification]) {
            [center addObserver:self selector:@selector(windowLayoutChanged:)
                           name:name object:window];
        }
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [super dealloc];
}

- (void)windowLayoutChanged:(NSNotification*)note {
    [self alignInWindow:[note object]];
}

- (void)alignInWindow:(NSWindow*)window {
    if (_aligning || NSIsEmptyRect(window.frame) || window.isMiniaturized ||
        (window.styleMask & NSWindowStyleMaskFullScreen)) return;

    // Resize notifications can precede AppKit's title-bar layout.
    _aligning = YES;
    [window layoutIfNeeded];
    [self positionButtonsInWindow:window];
    _aligning = NO;
}

- (void)positionButtonsInWindow:(NSWindow*)window {
    const std::array<NSButton*, 3> buttons = {
        [window standardWindowButton:NSWindowCloseButton],
        [window standardWindowButton:NSWindowMiniaturizeButton],
        [window standardWindowButton:NSWindowZoomButton],
    };
    for (NSButton* button : buttons) {
        if (button.window != window || NSIsEmptyRect(button.frame)) return;
    }

    // AppKit may recreate or reposition any one button. Keep the original
    // center-to-center distances in window coordinates, independent of its views.
    if (!_hasOffsets) {
        const NSRect close = [buttons[0] convertRect:buttons[0].bounds toView:nil];
        _centerOffsets[0] = 0;
        for (std::size_t i = 1; i < buttons.size(); ++i) {
            const NSRect rect = [buttons[i] convertRect:buttons[i].bounds toView:nil];
            _centerOffsets[i] = NSMidX(rect) - NSMidX(close);
            if (_centerOffsets[i] == 0) return;
        }
        _hasOffsets = YES;
    }

    // Same centerline as the Web navigation row: 5 pt + 3 pt + half of 24 pt.
    constexpr CGFloat centerFromTop = 20.0;
    const NSRect close = buttons[0].frame;
    const CGFloat closeCenterX = centerFromTop + (NSWidth(close) - NSHeight(close)) / 2;
    const CGFloat centerY = NSHeight(window.frame) - centerFromTop;
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        NSButton* button = buttons[i];
        const NSPoint windowCenter = NSMakePoint(closeCenterX + _centerOffsets[i], centerY);
        const NSPoint center = [button.superview convertPoint:windowCenter fromView:nil];
        const NSRect frame = button.frame;
        const NSPoint origin = NSMakePoint(center.x - NSWidth(frame) / 2,
                                           center.y - NSHeight(frame) / 2);
        if (!NSEqualPoints(origin, frame.origin)) [button setFrameOrigin:origin];
    }
}
@end

namespace acecode::desktop::detail {

void configure_mac_titlebar_buttons(NSWindow* window) {
    if (!window) return;
    static char layout_key;
    ACEWindowButtonLayout* layout = objc_getAssociatedObject(window, &layout_key);
    if (!layout) {
        layout = [[ACEWindowButtonLayout alloc] initWithWindow:window];
        objc_setAssociatedObject(window, &layout_key, layout, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [layout release];
    }
    if (window.styleMask & NSWindowStyleMaskFullScreen) return;
    for (NSWindowButton button : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
        [[window standardWindowButton:button] setHidden:NO];
    }
    [layout alignInWindow:window];
}

} // namespace acecode::desktop::detail
#endif
