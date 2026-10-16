#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include "computer_use/availability.hpp"
#include "computer_use/runtime.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <libproc.h>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using json = nlohmann::json;
namespace cu = acecode::computer_use;

// This fixture creates every input target itself. It never locates or operates
// another application's windows, and requires an explicit command-line mode.
@interface ACEInputCanvas : NSView
@property NSInteger drags;
@property CGFloat scrollAmount;
@property CGFloat horizontalScrollAmount;
@end
@implementation ACEInputCanvas
- (BOOL)isFlipped { return YES; }
- (BOOL)isAccessibilityElement { return YES; }
- (NSString*)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString*)accessibilityLabel { return @"Input canvas"; }
- (void)drawRect:(NSRect)dirty {
    [[NSColor colorWithSRGBRed:0.1 green:0.7 blue:0.2 alpha:1] setFill];
    NSRectFill(self.bounds);
}
- (void)mouseDown:(NSEvent*)event {}
- (void)mouseDragged:(NSEvent*)event { self.drags += 1; }
- (void)scrollWheel:(NSEvent*)event {
    self.scrollAmount += event.scrollingDeltaY;
    self.horizontalScrollAmount += event.scrollingDeltaX;
}
@end

@interface ACEInputFixture : NSObject <NSMenuDelegate>
@property NSWindow* window;
@property NSTextField* field;
@property NSTextField* focusField;
@property NSSecureTextField* password;
@property NSButton* button;
@property NSButton* checkbox;
@property ACEInputCanvas* canvas;
@property NSPanel* sheet;
@property NSPanel* obstruction;
@property NSWindow* otherWindow;
@property NSInteger clicks;
@property NSMutableArray<NSString*>* menuEvents;
- (void)clicked:(id)sender;
@end
@implementation ACEInputFixture
- (void)clicked:(id)sender { self.clicks += 1; }
- (void)menuWillOpen:(NSMenu*)menu { [self.menuEvents addObject:@"open"]; }
- (void)menuDidClose:(NSMenu*)menu { [self.menuEvents addObject:@"close"]; }
- (void)menu:(NSMenu*)menu willHighlightItem:(NSMenuItem*)item {
    [self.menuEvents addObject:item.title ?: @"no highlight"];
}
- (instancetype)init {
    if (!(self = [super init])) return nil;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 150, 640, 420)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"ACECode Computer Use Disposable Fixture";
    self.window.releasedWhenClosed = NO;
    self.field = [[NSTextField alloc] initWithFrame:NSMakeRect(30, 345, 570, 30)];
    self.field.accessibilityLabel = @"Editable fixture text";
    self.field.stringValue = @"initial value";
    [self.window.contentView addSubview:self.field];
    self.password = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(30, 295, 570, 30)];
    self.password.stringValue = @"fixture-password-must-never-leak";
    [self.window.contentView addSubview:self.password];
    self.button = [NSButton buttonWithTitle:@"Count click" target:self action:@selector(clicked:)];
    self.button.frame = NSMakeRect(30, 240, 160, 35);
    [self.window.contentView addSubview:self.button];
    self.checkbox = [NSButton checkboxWithTitle:@"Fixture check" target:nil action:nil];
    self.checkbox.frame = NSMakeRect(240, 240, 220, 35);
    [self.window.contentView addSubview:self.checkbox];
    self.focusField = [[NSTextField alloc] initWithFrame:NSMakeRect(470, 240, 130, 30)];
    self.focusField.accessibilityLabel = @"Other fixture text";
    self.focusField.stringValue = @"untouched";
    [self.window.contentView addSubview:self.focusField];
    self.canvas = [[ACEInputCanvas alloc] initWithFrame:NSMakeRect(30, 30, 570, 180)];
    self.canvas.menu = [[NSMenu alloc] initWithTitle:@"Fixture context menu"];
    self.menuEvents = [NSMutableArray new];
    self.canvas.menu.delegate = self;
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:@"Fixture menu action" action:@selector(clicked:) keyEquivalent:@""];
    item.target = self;
    [self.canvas.menu addItem:item];
    [self.window.contentView addSubview:self.canvas];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.field];
    [NSApp activateIgnoringOtherApps:YES];
    return self;
}
@end

namespace {
void main_sync(dispatch_block_t block) { dispatch_sync(dispatch_get_main_queue(), block); }
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
json call(json request) {
    auto result = cu::execute("macos-owned-fixture", request);
    require(result.value("success", false), request.dump() + ": " + result.value("output", json{}).dump());
    return result;
}
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }
json observe(int window, bool screenshot = true) {
    const json request{{"action", "get_window_state"}, {"window", window}, {"include_screenshot", screenshot}};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        auto result = cu::execute("macos-owned-fixture", request);
        if (result.value("success", false)) return result;
        const auto error = result.value("output", json::object()).value("error", std::string{});
        // WindowServer and AX publish a move on separate queues. Reobserve
        // their explicitly reported transition; never retry an input action.
        if ((error != "window_accessibility_unavailable" && error != "stale_window" && error != "stale_surface") ||
            std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error(request.dump() + ": " + result.value("output", json{}).dump());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
json element(const json& state, const std::string& name) {
    for (const auto& item : state["output"]["accessibility"]["elements"])
        if (item["name"] == name) return item;
    throw std::runtime_error("Missing AX element: " + name + "\n" + state["output"]["accessibility"].dump());
}
json target(const json& state, const char* action, int window) {
    return {{"action", action}, {"window", window}, {"observation_id", state["output"]["observation_id"]}};
}
void use_element(const json& state, const char* action, int window, const char* name, json extra = json::object()) {
    auto request = target(state, action, window);
    request["element_index"] = element(state, name)["index"];
    request.update(extra);
    call(request); settle();
}
json coordinate(const json& state, int window, const char* action, const json& item) {
    auto request = target(state, action, window);
    const auto& rect = item["bounds"];
    request.update({{"x", rect["x"].get<double>() + rect["width"].get<double>() / 2},
        {"y", rect["y"].get<double>() + rect["height"].get<double>() / 2}, {"screenshot_id", item["screenshot_id"]}});
    return request;
}
NSColor* screenshot_pixel(NSBitmapImageRep* image, int x, int y) {
    require(image.bitsPerSample == 8 && image.samplesPerPixel == 4, "Expected an eight-bit RGBA screenshot");
    NSUInteger pixel[4]{};
    [image getPixel:pixel atX:x y:y];
    const CGFloat components[]{pixel[0] / 255.0, pixel[1] / 255.0, pixel[2] / 255.0, pixel[3] / 255.0};
    // colorAtX:y: labels these sRGB samples as calibrated RGB on AppKit.
    // Retain the actual bitmap profile instead of applying a second conversion.
    return [[NSColor colorWithColorSpace:image.colorSpace components:components count:4]
        colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
}
void save_images(const json& state) {
    const char* folder = std::getenv("ACECODE_MAC_FIXTURE_ARTIFACTS");
    if (!folder) return;
    NSString* directory = [NSString stringWithUTF8String:folder];
    require([NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil],
        "Create optional native fixture artifact directory");
    for (std::size_t i = 0; i < state["attachments"].size(); ++i) {
        const auto url = state["attachments"][i]["data_url"].get<std::string>();
        NSData* data = [[NSData alloc] initWithBase64EncodedString:[NSString stringWithUTF8String:url.substr(22).c_str()] options:0];
        NSString* path = [directory stringByAppendingPathComponent:[NSString stringWithFormat:@"surface-%zu.png", i]];
        require([data writeToFile:path atomically:YES], "Write owned-window screenshot artifact");
    }
    std::cout << "Owned-window screenshots: " << folder << '\n';
}
void check_image(const json& state) {
    require(!state["attachments"].empty(), "Screenshot attachment is missing");
    const auto url = state["attachments"][0]["data_url"].get<std::string>();
    require(url.rfind("data:image/png;base64,", 0) == 0, "PNG attachment contract");
    NSString* encoded = [NSString stringWithUTF8String:url.substr(22).c_str()];
    NSData* data = [[NSData alloc] initWithBase64EncodedString:encoded options:0];
    NSBitmapImageRep* image = [NSBitmapImageRep imageRepWithData:data];
    const auto& geometry = state["output"]["screenshots"][0];
    require(image && image.pixelsWide == geometry["width"] && image.pixelsHigh == geometry["height"], "PNG dimensions match returned geometry");
    require(geometry["capture_method"] == "screencapturekit", "Native capture method");
    // Test actual orientation and content mapping, not merely metadata arithmetic.
    const auto canvas = element(state, "Input canvas")["bounds"];
    const int x = canvas["x"].get<double>() + canvas["width"].get<double>() * 0.8;
    const int y = canvas["y"].get<double>() + canvas["height"].get<double>() * 0.8;
    NSColor* color = screenshot_pixel(image, x, y);
    require(color.greenComponent > 0.55 && color.redComponent < 0.25 && color.blueComponent < 0.35, "AX bounds map to actual screenshot pixels");
}
void check_pointer_color(const json& state, double red, double green, double blue) {
    const auto& screenshot = state["output"]["screenshots"][0];
    const auto& cursor = screenshot["cursor"];
    require(cursor.value("visible", false) && cursor["source"] == "agent", "Themed pointer metadata");
    const auto url = state["attachments"][0]["data_url"].get<std::string>();
    NSData* data = [[NSData alloc] initWithBase64EncodedString:[NSString stringWithUTF8String:url.substr(22).c_str()] options:0];
    NSBitmapImageRep* image = [NSBitmapImageRep imageRepWithData:data];
    const double scale_x = screenshot["width"].get<double>() / screenshot["native_width"].get<double>();
    const double scale_y = screenshot["height"].get<double>() / screenshot["native_height"].get<double>();
    const int x = cursor["x"].get<double>() + 3 * scale_x, y = cursor["y"].get<double>() + 10 * scale_y;
    require(x >= 0 && y >= 0 && x < image.pixelsWide && y < image.pixelsHigh, "Pointer sample inside screenshot");
    NSColor* color = screenshot_pixel(image, x, y);
    require(std::abs(color.redComponent - red) < 0.08 && std::abs(color.greenComponent - green) < 0.08 &&
        std::abs(color.blueComponent - blue) < 0.08, "Actual PNG pointer pixels match configured theme color: got " +
        std::to_string(color.redComponent) + "," + std::to_string(color.greenComponent) + "," +
        std::to_string(color.blueComponent) + " at " + std::to_string(x) + "," + std::to_string(y));
}
NSArray<NSDictionary*>* pointer_windows() {
    // Identify only this fixture's real helper, not another app or a pointer
    // drawn into a tool attachment. No production test hook is involved.
    NSString* expected = [NSBundle.mainBundle.executablePath.stringByDeletingLastPathComponent
        stringByAppendingPathComponent:@"acecode-computer-use"];
    NSArray* windows = CFBridgingRelease(CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
    NSMutableArray* matches = [NSMutableArray new];
    for (NSDictionary* window in windows) {
        const pid_t pid = [window[(__bridge NSString*)kCGWindowOwnerPID] intValue];
        proc_bsdinfo info{};
        char path[PROC_PIDPATHINFO_MAXSIZE]{};
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info) ||
            info.pbi_ppid != getpid() || proc_pidpath(pid, path, sizeof(path)) <= 0 ||
            ![expected isEqualToString:[NSString stringWithUTF8String:path]]) continue;
        [matches addObject:window];
    }
    return matches;
}
void check_overlay(ACEInputFixture* fixture, NSView* target, double red, double green, double blue) {
    require(CGPreflightScreenCaptureAccess(), "Independent overlay capture requires Screen Recording for the fixture launch");
    NSArray<NSDictionary*>* windows = pointer_windows();
    require(windows.count == 1, "Exactly one real helper overlay is on the desktop");
    NSDictionary* overlay = windows.firstObject;
    const CGWindowID id = [overlay[(__bridge NSString*)kCGWindowNumber] unsignedIntValue];
    require([overlay[(__bridge NSString*)kCGWindowAlpha] doubleValue] > 0.99, "Native overlay is not transparent at window level");
    require([overlay[(__bridge NSString*)kCGWindowLayer] integerValue] >= NSStatusWindowLevel, "Native overlay is above normal windows");
    CGRect bounds{};
    require(CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)overlay[(__bridge NSString*)kCGWindowBounds], &bounds),
        "Native overlay has WindowServer geometry");
    __block NSRect expected;
    __block bool focused;
    main_sync(^{
        expected = [fixture.window convertRectToScreen:[target convertRect:target.bounds toView:nil]];
        expected.origin.y = NSScreen.screens.firstObject.frame.size.height - NSMaxY(expected);
        focused = fixture.window.isKeyWindow && NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == getpid();
    });
    require(focused, "Native overlay does not steal application or window focus");
    require(NSPointInRect(NSMakePoint(bounds.origin.x + 10, bounds.origin.y + 10), expected),
        "Actual overlay hotspot is over the expected fixture control");

    struct Capture {
        dispatch_semaphore_t ready = dispatch_semaphore_create(0);
        __strong SCShareableContent* content = nil;
        __strong NSBitmapImageRep* image = nil;
        __strong NSError* error = nil;
    };
    auto capture = std::make_shared<Capture>();
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES
        completionHandler:^(SCShareableContent* content, NSError* error) {
            capture->content = content; capture->error = error; dispatch_semaphore_signal(capture->ready);
        }];
    require(dispatch_semaphore_wait(capture->ready, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) == 0,
        "Independent overlay enumeration completes");
    SCWindow* window = nil;
    for (SCWindow* candidate in capture->content.windows) if (candidate.windowID == id) window = candidate;
    require(window && window.isOnScreen, "ScreenCaptureKit independently sees the on-screen helper overlay");
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
    SCStreamConfiguration* config = [SCStreamConfiguration new];
    config.width = std::lround(bounds.size.width * filter.pointPixelScale);
    config.height = std::lround(bounds.size.height * filter.pointPixelScale);
    config.showsCursor = NO;
    config.ignoreShadowsSingleWindow = YES;
    if (@available(macOS 14.2, *)) config.includeChildWindows = NO;
    [SCScreenshotManager captureImageWithFilter:filter configuration:config
        completionHandler:^(CGImageRef image, NSError* error) {
            if (image) capture->image = [[NSBitmapImageRep alloc] initWithCGImage:image];
            capture->error = error; dispatch_semaphore_signal(capture->ready);
        }];
    require(dispatch_semaphore_wait(capture->ready, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) == 0 && capture->image,
        "Independent capture of actual overlay pixels completes");
    NSBitmapImageRep* image = capture->image;
    const int x = std::lround(13 * image.pixelsWide / bounds.size.width);
    const int y = std::lround(20 * image.pixelsHigh / bounds.size.height);
    NSColor* color = screenshot_pixel(image, x, y);
    require(color.alphaComponent > 0.9 && std::abs(color.redComponent - red) < 0.08 &&
        std::abs(color.greenComponent - green) < 0.08 && std::abs(color.blueComponent - blue) < 0.08,
        "Actual WindowServer overlay pixels match the theme, independently of pointer_composite; got " +
        std::to_string(color.redComponent) + "," + std::to_string(color.greenComponent) + "," +
        std::to_string(color.blueComponent) + ", alpha=" + std::to_string(color.alphaComponent));
    const char* folder = std::getenv("ACECODE_MAC_FIXTURE_ARTIFACTS");
    if (folder) {
        NSString* directory = [NSString stringWithUTF8String:folder];
        require([NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil],
            "Create optional overlay artifact directory");
        NSData* png = [image representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        require([png writeToFile:[directory stringByAppendingPathComponent:@"native-overlay.png"] atomically:YES],
            "Save independently captured helper overlay");
    }
}
void check_overlay_removed() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pointer_windows().count != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(pointer_windows().count == 0, "Release removes the real WindowServer overlay");
}
int run(ACEInputFixture* fixture, bool inputs) {
    try {
        const auto readiness = cu::availability();
        require(readiness.value("supported", false), "macOS 14 or later required");
        if (!readiness.value("ready", false)) {
            std::cerr << "Native fixture not verified; required OS permissions: " << readiness.dump() << '\n';
            return 77;
        }
        if (inputs && !CGPreflightScreenCaptureAccess()) {
            std::cerr << "Native overlay not verified; independent fixture capture requires Screen Recording.\n";
            return 77;
        }
        __block int native_window;
        main_sync(^{ native_window = static_cast<int>(fixture.window.windowNumber); });
        const int window = native_window;
        cu::set_enabled(true);
        call({{"action", "get_window"}, {"window", window}});
        const auto listed = call({{"action", "list_windows"}});
        bool listed_own_window = false;
        for (const auto& item : listed["output"]["windows"]) if (item["id"] == window) listed_own_window = true;
        require(listed_own_window, "Window discovery includes the owned fixture");
        const auto apps = call({{"action", "list_apps"}});
        const std::string bundle_path = NSBundle.mainBundle.bundlePath.UTF8String;
        if ([NSBundle.mainBundle.bundlePath.pathExtension isEqualToString:@"app"]) {
            bool found = false;
            for (const auto& item : apps["output"]["apps"]) if (item["id"] == bundle_path) found = true;
            require(found, "App discovery includes the exact fixture bundle");
            const auto launched = call({{"action", "launch_app"}, {"app", bundle_path}});
            require(launched["output"]["pid"] == getpid(), "Launch uses the already-running exact fixture bundle");
        }
        call({{"action", "activate_window"}, {"window", window}});
        auto state = observe(window);
        check_image(state);
        require(state["output"]["accessibility"].dump().find("fixture-password-must-never-leak") == std::string::npos, "Password is never exposed through AX");
        require(state["output"]["accessibility"].contains("focused_element"), "Focused element is exposed");
        std::cout << "Passed native discovery, activation, screenshot pixels and protected AX observation." << std::endl;
        if (!inputs) { cu::shutdown(); return 0; }

        // A real input-receiving panel must still block clicks, even when a
        // system-owned transparent Dock backing window is above the desktop.
        main_sync(^{
            const NSRect frame = [fixture.window convertRectToScreen:
                [fixture.button convertRect:fixture.button.bounds toView:nil]];
            fixture.obstruction = [[NSPanel alloc] initWithContentRect:NSInsetRect(frame, -5, -5)
                styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                backing:NSBackingStoreBuffered defer:NO];
            fixture.obstruction.releasedWhenClosed = NO;
            fixture.obstruction.level = NSFloatingWindowLevel;
            [fixture.obstruction orderFrontRegardless];
        });
        settle();
        auto obscured = target(state, "click", window);
        obscured["element_index"] = element(state, "Count click")["index"];
        require(cu::execute("macos-owned-fixture", obscured)["output"]["error"] == "target_obscured",
            "A covering input window rejects the click");
        main_sync(^{ [fixture.obstruction close]; });
        state = observe(window);
        use_element(state, "click", window, "Count click");
        __block NSInteger count;
        main_sync(^{ count = fixture.clicks; });
        require(count == 1, "AX-index mouse click reaches button");
        auto reused = target(state, "click", window);
        reused["element_index"] = element(state, "Count click")["index"];
        require(cu::execute("macos-owned-fixture", reused)["output"]["error"] == "stale_observation", "Observation is one-use");
        state = observe(window);
        require(state["output"]["screenshots"][0]["cursor"].value("visible", false), "ACE pointer is composed after clicking");
        check_pointer_color(state, 37.0 / 255, 99.0 / 255, 235.0 / 255);
        check_overlay(fixture, fixture.button, 37.0 / 255, 99.0 / 255, 235.0 / 255);
        check_image(state);
        auto click = coordinate(state, window, "click", element(state, "Count click"));
        call(click); settle();
        main_sync(^{ count = fixture.clicks; });
        require(count == 2, "Click passes through the visible native overlay to the same button");
        state = observe(window, false);
        use_element(state, "perform_secondary_action", window, "Count click", {{"secondary_action", "invoke"}});
        main_sync(^{ count = fixture.clicks; });
        require(count == 3, "Advertised AX invoke reaches button");
        state = observe(window, false);
        use_element(state, "perform_secondary_action", window, "Fixture check", {{"secondary_action", "toggle"}});
        __block bool checked;
        main_sync(^{ checked = fixture.checkbox.state == NSControlStateValueOn; });
        require(checked, "Advertised AX toggle updates checkbox");
        state = observe(window, false);
        use_element(state, "set_value", window, "Editable fixture text", {{"value", "native AX value"}});
        __block std::string value;
        main_sync(^{ value = fixture.field.stringValue.UTF8String; });
        require(value == "native AX value", "Writable AX control updates");
        state = observe(window, false);
        use_element(state, "click", window, "Editable fixture text");
        call({{"action", "release"}});
        check_overlay_removed();
        state = observe(window, false);
        auto key = target(state, "press_key", window); key["key"] = "Cmd+A"; call(key); settle();
        check_overlay(fixture, fixture.field, 37.0 / 255, 99.0 / 255, 235.0 / 255);
        call({{"action", "release"}});
        check_overlay_removed();
        state = observe(window, false);
        auto type = target(state, "type_text", window); type["text"] = "ACE \u4e2d\u6587 \U0001f642"; call(type); settle();
        check_overlay(fixture, fixture.field, 37.0 / 255, 99.0 / 255, 235.0 / 255);
        main_sync(^{ value = fixture.field.stringValue.UTF8String; });
        require(value == "ACE \u4e2d\u6587 \U0001f642", "Unicode keyboard input and Cmd+A preserve text exactly; actual value: " + value);
        const auto press = [&](const char* chord) {
            const auto current = observe(window, false);
            auto request = target(current, "press_key", window); request["key"] = chord;
            call(request); settle();
            __block NSRange selected;
            main_sync(^{ selected = fixture.field.currentEditor.selectedRange; });
            return selected;
        };
        auto selected = press("Cmd+Left");
        require(selected.location == 0 && selected.length == 0, "Command chord moves to line start");
        selected = press("Shift+Right");
        require(selected.location == 0 && selected.length == 1, "Shift chord extends the selection");
        selected = press("Option+Right");
        require(selected.location > 1 && selected.length == 0, "Option chord moves to a word boundary");
        selected = press("Ctrl+A");
        require(selected.location == 0 && selected.length == 0, "Control chord reaches the text responder");
        std::cout << "Passed native clicks, AX controls, Unicode and Command/Option/Control/Shift chords." << std::endl;

        state = observe(window);
        auto scroll = coordinate(state, window, "scroll", element(state, "Input canvas"));
        scroll.update({{"scrollX", 80}, {"scrollY", 120}}); call(scroll); settle();
        __block CGFloat scrolled, horizontal_scroll;
        main_sync(^{ scrolled = fixture.canvas.scrollAmount; horizontal_scroll = fixture.canvas.horizontalScrollAmount; });
        require(scrolled < 0, "Positive vertical scroll sends downward native pixels");
        require(horizontal_scroll < 0, "Positive horizontal scroll sends rightward native pixels");
        state = observe(window);
        auto drag = coordinate(state, window, "drag", element(state, "Input canvas"));
        drag["from_x"] = drag["x"]; drag["from_y"] = drag["y"];
        drag["to_x"] = drag["x"].get<double>() + 50; drag["to_y"] = drag["y"].get<double>() + 20;
        drag.erase("x"); drag.erase("y"); call(drag); settle();
        __block NSInteger drags;
        main_sync(^{ drags = fixture.canvas.drags; });
        require(drags > 0, "Real mouse drag reaches fixture");
        require(!CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft), "Drag releases mouse button");
        cu::set_pointer_appearance("plain", "#ff6600");
        state = observe(window);
        require(state["output"]["screenshots"][0]["cursor"].value("visible", false), "Pointer survives appearance updates");
        check_pointer_color(state, 1, 0.4, 0);
        check_overlay(fixture, fixture.canvas, 1, 0.4, 0);
        check_image(state);
        std::cout << "Passed native scrolling, drag and ACE/plain pointer pixels." << std::endl;

        auto stale = coordinate(state, window, "click", element(state, "Count click"));
        main_sync(^{ auto frame = fixture.window.frame; frame.origin.x += 10; [fixture.window setFrame:frame display:YES]; });
        require(!cu::execute("macos-owned-fixture", stale).value("success", true), "Window movement invalidates observation");
        state = observe(window, false);
        auto focus = target(state, "type_text", window); focus["text"] = "must-not-type";
        main_sync(^{ [fixture.window makeFirstResponder:fixture.focusField]; });
        require(cu::execute("macos-owned-fixture", focus)["output"]["error"] == "focus_changed", "Changed focus rejects keyboard input");
        main_sync(^{ value = fixture.focusField.stringValue.UTF8String; });
        require(value == "untouched", "Rejected keyboard input leaves the other control unchanged");
        main_sync(^{
            fixture.otherWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(780, 180, 240, 180)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            fixture.otherWindow.releasedWhenClosed = NO;
            NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(10, 70, 220, 30)];
            [fixture.otherWindow.contentView addSubview:field];
            [fixture.otherWindow makeKeyAndOrderFront:nil];
            [fixture.otherWindow makeFirstResponder:field];
        });
        state = observe(window, false);
        auto other = target(state, "type_text", window); other["text"] = "must-not-type-in-other-window";
        require(cu::execute("macos-owned-fixture", other)["output"]["error"] == "focus_changed", "Same-process other window rejects keyboard input");
        std::cout << "Passed stale geometry and changed-focus rejection." << std::endl;
        call({{"action", "activate_window"}, {"window", window}});
        main_sync(^{ [fixture.otherWindow close]; [fixture.window makeFirstResponder:fixture.field]; });
        main_sync(^{
            fixture.sheet = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 320, 160)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            NSButton* button = [NSButton buttonWithTitle:@"Sheet owned button" target:fixture action:@selector(clicked:)];
            button.frame = NSMakeRect(30, 50, 230, 40); [fixture.sheet.contentView addSubview:button];
            [fixture.window beginSheet:fixture.sheet completionHandler:nil];
        });
        settle();
        state = observe(window);
        require(state["output"]["surfaces"].size() > 1, "Owned sheet receives a separate surface");
        call(coordinate(state, window, "click", element(state, "Sheet owned button"))); settle();
        main_sync(^{ count = fixture.clicks; [fixture.window endSheet:fixture.sheet]; [fixture.sheet orderOut:nil]; });
        require(count == 4, "Related sheet input reaches exact child window");
        settle();
        state = observe(window);
        auto context = coordinate(state, window, "click", element(state, "Input canvas"));
        context["mouse_button"] = "right"; call(context); settle();
        state = observe(window);
        require(state["output"]["surfaces"].size() > 1, "Owned context menu receives a separate surface");
        save_images(state);
        call(coordinate(state, window, "click", element(state, "Fixture menu action"))); settle();
        const auto menu_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            main_sync(^{ count = fixture.clicks; });
            if (count >= 5) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < menu_deadline);
        __block std::string menu_events;
        main_sync(^{ count = fixture.clicks; menu_events = [fixture.menuEvents componentsJoinedByString:@", "].UTF8String; });
        require(count == 5, "Context menu coordinate input reaches the owned action; click count: " + std::to_string(count) +
            "; menu events: " + menu_events + "; target: " + element(state, "Fixture menu action").dump());
        std::cout << "Passed owned sheet and context-menu capture/input." << std::endl;
        call({{"action", "release"}});
        check_overlay_removed();
        state = observe(window);
        require(!state["output"]["screenshots"][0]["cursor"].value("visible", true), "Release removes the pointer from screenshots");
        cu::shutdown();
        std::cout << "Native macOS fixture passed: ScreenCaptureKit pixels, AX, protected text, clicks, Unicode, chord, scroll, drag, controls, stale observations, focus, sheet and pointer.\n";
        return 0;
    } catch (const std::exception& error) {
        cu::shutdown(); std::cerr << error.what() << '\n'; return 1;
    }
}
}

int main(int argc, char** argv) {
    if (argc != 2 || (std::string(argv[1]) != "--run-owned-window" && std::string(argv[1]) != "--observe-owned-window")) {
        std::cerr << "Use --observe-owned-window or --run-owned-window. Input is limited to this fixture's disposable window.\n";
        return 2;
    }
    @autoreleasepool {
        [NSApplication sharedApplication];
        if ([NSWorkspace.sharedWorkspace.frontmostApplication.bundleIdentifier isEqualToString:@"com.apple.loginwindow"]) {
            std::cerr << "Native overlay/input verification requires an unlocked desktop; not verified.\n";
            return 77;
        }
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSMenu* menu = [NSMenu new];
        NSMenuItem* app = [menu addItemWithTitle:@"Fixture" action:nil keyEquivalent:@""];
        app.submenu = [NSMenu new];
        [app.submenu addItemWithTitle:@"Quit fixture" action:@selector(terminate:) keyEquivalent:@"q"];
        NSMenuItem* edit = [menu addItemWithTitle:@"Edit" action:nil keyEquivalent:@""];
        edit.submenu = [[NSMenu alloc] initWithTitle:@"Edit"];
        [edit.submenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
        NSApp.mainMenu = menu;
        ACEInputFixture* fixture = [ACEInputFixture new];
        const bool inputs = std::string(argv[1]) == "--run-owned-window";
        std::thread([fixture, inputs] {
            @autoreleasepool {
                settle();
                const int result = run(fixture, inputs);
                dispatch_async(dispatch_get_main_queue(), ^{ [fixture.window close]; std::exit(result); });
            }
        }).detach();
        [NSApp run];
    }
    return 1;
}
