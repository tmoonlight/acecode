// Opt-in smoke for the Desktop shell's native iOS Simulator view:
//   ios_simulator_view_mac_smoke <booted-udid> <xcode>/Contents/Developer
// Drives the real IosSimulatorView in an off-screen window and checks the
// layer state (IOSurface contents, geometry, occlusion mask, visibility and
// unmount). Prints SMOKE_OK and exits 0 on success.
#include "desktop/ios_simulator_view.hpp"

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

#include <cmath>
#include <cstdio>
#include <string>

using acecode::desktop::IosSimulatorView;
using acecode::desktop::IosSimulatorViewLayout;

@interface SmokeFlippedView : NSView
@end

@implementation SmokeFlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "[ok]  " : "[FAIL]", what);
    if (!condition) ++g_failures;
}

bool spin_until(double seconds, bool (^done)(void)) {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (!done()) {
        if (deadline.timeIntervalSinceNow <= 0) return false;
        [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    return true;
}

NSView* screen_view(NSView* content) {
    for (NSView* view in content.subviews)
        if ([NSStringFromClass([view class]) isEqualToString:@"ACECodeIosSimulatorScreenView"]) return view;
    return nil;
}

IosSimulatorViewLayout layout_for(const std::string& udid, const std::string& developer_dir, bool visible) {
    IosSimulatorViewLayout layout;
    layout.udid = udid;
    layout.developer_dir = developer_dir;
    layout.frame = {40, 30, 390, 844};
    layout.visible = visible;
    layout.corner_radius = 48;
    layout.occlusion_rects = {{0, 0, 120, 60}};
    return layout;
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) {
            std::fprintf(stderr, "usage: %s <booted-udid> <developer-dir>\n", argv[0]);
            return 64;
        }
        const std::string udid = argv[1];
        const std::string developer_dir = argv[2];
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 900)
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        SmokeFlippedView* content = [[SmokeFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 480, 900)];
        content.wantsLayer = YES;
        window.contentView = content;
        [window setFrameOrigin:NSMakePoint(-20000, -20000)];  // layer state only, no pixels
        [window orderFrontRegardless];

        nlohmann::json last = nlohmann::json::object();
        nlohmann::json* last_state = &last;  // blocks capture the pointer
        IosSimulatorView view((__bridge void*)window,
                              [last_state](const nlohmann::json& state) { *last_state = state; });
        check(view.supported(), "CoreSimulator is installed");

        std::string error;
        auto bad = layout_for(udid, "/nonexistent/Contents/Developer", true);
        check(!view.set_layout(bad, &error) && !error.empty(), "a non-Xcode developer_dir is refused");

        check(view.set_layout(layout_for(udid, developer_dir, true), &error), "layout accepted");
        const bool rendering = spin_until(15, ^{ return last_state->value("rendering", false); });
        std::printf("state: %s\n", last.dump().c_str());
        check(rendering, "the view renders the simulator");

        NSView* screen = screen_view(content);
        check(screen != nil && !screen.hidden, "screen view is placed over the content");
        if (screen) {
            check(NSEqualRects(screen.frame, NSMakeRect(40, 30, 390, 844)), "screen view uses the DOM frame");
            check(std::fabs(screen.layer.cornerRadius - 48) < 0.01, "rounded device corners");
            check(screen.layer.mask != nil, "menus over the screen are cut out");
            check([screen hitTest:NSMakePoint(100, 100)] == nil, "input passes through to the page");
            CALayer* layer = screen.layer.sublayers.firstObject;
            id contents = layer.contents;
            const bool is_surface = contents && CFGetTypeID((__bridge CFTypeRef)contents) == IOSurfaceGetTypeID();
            check(is_surface, "layer contents are the simulator IOSurface (zero copy)");
            if (is_surface) {
                IOSurfaceRef surface = (__bridge IOSurfaceRef)contents;
                check(static_cast<double>(IOSurfaceGetWidth(surface)) == last.value("pixel_width", 0.0),
                      "reported pixel size matches the surface");
                const double expected_scale = std::fmin(390.0 / IOSurfaceGetWidth(surface),
                                                        844.0 / IOSurfaceGetHeight(surface));
                if (last.value("rotation", 0) == 0) {
                    check(std::fabs(layer.bounds.size.width - IOSurfaceGetWidth(surface) * expected_scale) < 0.5,
                          "layer is aspect-fitted");
                    check(CATransform3DIsIdentity(layer.transform), "portrait needs no rotation");
                }
            }
        }

        check(view.set_layout(layout_for(udid, developer_dir, false), &error), "hide accepted");
        spin_until(2, ^{ return !last_state->value("rendering", true); });
        check(!last.value("rendering", true) && (screen == nil || screen.hidden), "hidden layout hides the view");
        check(last.value("attached", false), "hiding keeps the device attached");

        check(view.set_layout(layout_for(udid, developer_dir, true), &error), "show accepted");
        check(spin_until(2, ^{ return last_state->value("rendering", false); }), "shows again without re-attaching");

        view.unmount();
        check(!last.value("attached", true) && screen_view(content) == nil, "unmount removes the view");

        auto missing = layout_for("00000000-0000-0000-0000-000000000000", developer_dir, true);
        check(view.set_layout(missing, &error), "unknown device layout accepted");
        spin_until(10, ^{ return !last_state->value("error", std::string()).empty(); });
        check(last.value("error", std::string()).find("DEVICE_NOT_FOUND") != std::string::npos &&
                  !last.value("rendering", true),
              "an unknown device reports DEVICE_NOT_FOUND");
        view.unmount();

        if (g_failures) {
            std::printf("SMOKE_FAILED %d\n", g_failures);
            return 1;
        }
        std::printf("SMOKE_OK\n");
        return 0;
    }
}
