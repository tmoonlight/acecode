#include "macos_native.hpp"

#include <algorithm>
#include <libproc.h>
#include <set>
#include <unistd.h>

namespace acecode::computer_use::macos {
namespace {
std::uint64_t start_time(pid_t pid) {
    proc_bsdinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) return 0;
    return info.pbi_start_tvsec * 1000000ULL + info.pbi_start_tvusec;
}
AX bind_window(const Window& value) {
    AX app(AXUIElementCreateApplication(value.pid));
    AXUIElementSetMessagingTimeout(app.get(), 0.3f);
    std::vector<AX> matches;
    auto candidates = ax_children(app.get(), kAXWindowsAttribute);
    for (std::size_t i = 0; i < candidates.size() && i < 64; ++i) {
        const auto candidate = candidates[i];
        if (same_rect(ax_bounds(candidate.get()), value.rect)) matches.push_back(candidate);
        for (auto child : ax_children(candidate.get())) {
            const auto role = string_attribute(child.get(), kAXRoleAttribute, 64);
            if ((role == "AXSheet" || role == "AXWindow") && candidates.size() < 64) candidates.push_back(std::move(child));
        }
    }
    if (matches.size() > 1 && !value.title.empty()) {
        matches.erase(std::remove_if(matches.begin(), matches.end(), [&](const AX& element) {
            return string_attribute(element.get(), kAXTitleAttribute) != value.title;
        }), matches.end());
    }
    if (matches.size() != 1) throw Error("window_accessibility_unavailable",
        "Could not uniquely bind this screenshot window to its accessibility window. Activate it and observe again.");
    return matches.front();
}
}

json permissions(const std::string& request) {
    if (!request.empty() && request != "accessibility" && request != "screen_recording")
        throw Error("invalid_permission", "Expected accessibility or screen_recording.");
    if (request == "accessibility") {
        NSDictionary* options = @{(__bridge NSString*)kAXTrustedCheckOptionPrompt: @YES};
        AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
    } else if (request == "screen_recording") {
        CGRequestScreenCaptureAccess();
    }
    const bool ax = AXIsProcessTrusted();
    const bool screen = CGPreflightScreenCaptureAccess();
    if ((request == "accessibility" && !ax) || (request == "screen_recording" && !screen)) {
        NSString* pane = request == "accessibility" ? @"Privacy_Accessibility" : @"Privacy_ScreenCapture";
        [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:[@"x-apple.systempreferences:com.apple.preference.security?" stringByAppendingString:pane]]];
    }
    return {{"accessibility", ax ? "granted" : "required"},
        {"screen_recording", screen ? "granted" : "required"},
        {"ready", ax && screen}, {"helper_pid", getpid()},
        {"helper_path", text(NSBundle.mainBundle.executablePath)},
        {"permission_app", "Authorize the application shown by macOS for this ACECode launch, then refresh permissions."}};
}
void require_accessibility() {
    if (!AXIsProcessTrusted()) throw Error("accessibility_permission_required",
        "Allow Accessibility for ACECode in System Settings > Privacy & Security, then refresh Computer Use permissions.");
}
void require_interactive_session() {
    Ref<CFDictionaryRef> session(CGSessionCopyCurrentDictionary());
    if (!session) throw Error("desktop_unavailable", "Computer Use requires an active local graphical login session.");
    auto active = CFDictionaryGetValue(session.get(), kCGSessionOnConsoleKey);
    if (!active || !CFEqual(active, kCFBooleanTrue))
        throw Error("desktop_unavailable", "This login session is not the active console desktop.");
    // The lock window is a WindowServer surface owned by loginwindow. Avoid
    // private CGS lock-state keys and refuse input if a lock screen is present.
    Ref<CFArrayRef> list(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
    for (NSDictionary* item in (__bridge NSArray*)list.get()) {
        if ([item[(__bridge NSString*)kCGWindowOwnerName] isEqualToString:@"loginwindow"] &&
            [item[(__bridge NSString*)kCGWindowLayer] integerValue] >= 100)
            throw Error("desktop_locked", "Unlock the Mac before using Computer Use.");
    }
}
std::vector<Window> windows(bool bind_ax) {
    if (bind_ax) require_accessibility();
    std::vector<Window> result;
    Ref<CFArrayRef> list(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
    for (NSDictionary* item in (__bridge NSArray*)list.get()) {
        Window value;
        value.id = [item[(__bridge NSString*)kCGWindowNumber] unsignedIntValue];
        value.pid = [item[(__bridge NSString*)kCGWindowOwnerPID] intValue];
        if (value.pid <= 0 || value.pid == getpid() || pointer_owns(value.id) || [item[(__bridge NSString*)kCGWindowAlpha] doubleValue] <= 0) continue;
        if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)item[(__bridge NSString*)kCGWindowBounds], &value.rect) ||
            !valid_rect(screen_rect(value.rect))) continue;
        NSRunningApplication* application = [NSRunningApplication runningApplicationWithProcessIdentifier:value.pid];
        if (!application || application.terminated || application.activationPolicy == NSApplicationActivationPolicyProhibited) continue;
        value.process_start = start_time(value.pid);
        if (!value.process_start) continue;
        value.app = text(application.bundleIdentifier ?: application.bundleURL.path ?: application.executableURL.path);
        value.title = text(item[(__bridge NSString*)kCGWindowName]);
        if (bind_ax) {
            try { value.root = bind_window(value); }
            catch (const Error&) { continue; }
        }
        result.push_back(std::move(value));
        if (result.size() >= 256) break;
    }
    return result;
}
Window window(CGWindowID id, bool bind_ax) {
    if (!id) throw Error("invalid_window", "Use a window id returned by computer_list_windows.");
    for (auto value : windows(false)) {
        if (value.id != id) continue;
        if (bind_ax) { require_accessibility(); value.root = bind_window(value); }
        return value;
    }
    throw Error("window_unavailable", "Window is closed, hidden or unavailable. List windows and observe again.");
}
json describe(const Window& value) {
    return {{"id", value.id}, {"pid", value.pid}, {"app", value.app}, {"title", value.title},
        {"bounds", {{"x", value.rect.origin.x}, {"y", value.rect.origin.y}, {"width", value.rect.size.width}, {"height", value.rect.size.height}}},
        {"coordinate_space", "screen_points"}};
}
json list_apps() {
    NSMutableDictionary<NSString*, NSMutableDictionary*>* apps = [NSMutableDictionary dictionary];
    const auto add = [&](NSURL* url, NSRunningApplication* running) {
        NSBundle* bundle = [NSBundle bundleWithURL:url];
        if (!bundle.bundleIdentifier || !bundle.executableURL) return;
        NSString* key = url.URLByStandardizingPath.path;
        if (!apps[key]) apps[key] = [@{@"id": key, @"bundle_id": bundle.bundleIdentifier,
            @"displayName": bundle.infoDictionary[@"CFBundleDisplayName"] ?: bundle.infoDictionary[@"CFBundleName"] ?: url.lastPathComponent,
            @"isRunning": @NO} mutableCopy];
        if (running) apps[key][@"isRunning"] = @YES;
    };
    for (NSRunningApplication* app in NSWorkspace.sharedWorkspace.runningApplications)
        if (app.bundleURL && app.activationPolicy != NSApplicationActivationPolicyProhibited) add(app.bundleURL, app);
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    for (NSString* path in @[@"/Applications", @"/System/Applications", [NSHomeDirectory() stringByAppendingPathComponent:@"Applications"]]) {
        NSDirectoryEnumerator* enumerator = [NSFileManager.defaultManager enumeratorAtURL:[NSURL fileURLWithPath:path]
            includingPropertiesForKeys:@[NSURLIsDirectoryKey] options:NSDirectoryEnumerationSkipsHiddenFiles errorHandler:nil];
        for (NSURL* url in enumerator) {
            if (Clock::now() >= deadline || apps.count >= 2048) break;
            if ([url.pathExtension.lowercaseString isEqualToString:@"app"]) { add(url, nil); [enumerator skipDescendants]; }
            else if (enumerator.level > 3) [enumerator skipDescendants];
        }
    }
    json output = json::array();
    const auto running_windows = windows();
    for (NSString* key in [[apps allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
        NSDictionary* app = apps[key];
        json entry{{"id", text(app[@"id"])}, {"bundle_id", text(app[@"bundle_id"])}, {"displayName", text(app[@"displayName"])},
            {"isRunning", [app[@"isRunning"] boolValue]}, {"windows", json::array()}};
        for (const auto& value : running_windows) {
            auto running = [NSRunningApplication runningApplicationWithProcessIdentifier:value.pid];
            if ([running.bundleURL.URLByStandardizingPath.path isEqualToString:key]) entry["windows"].push_back(describe(value));
        }
        output.push_back(std::move(entry));
    }
    return output;
}
json launch_app(const std::string& application) {
    NSString* name = ns(application);
    if (!name.length || application.find('\0') != std::string::npos)
        throw Error("invalid_application", "Specify a discovered bundle id or absolute .app path.");
    NSURL* url = nil;
    if ([name isAbsolutePath]) {
        if (![name.pathExtension.lowercaseString isEqualToString:@"app"])
            throw Error("invalid_application", "An absolute application path must identify a .app bundle.");
        url = [NSURL fileURLWithPath:name];
    } else {
        if ([name containsString:@"/"] || [name containsString:@":"] || [name rangeOfCharacterFromSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].location != NSNotFound)
            throw Error("invalid_application", "Expected a bundle id, not a shell command or URL.");
        url = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:name];
    }
    if (!url || ![NSBundle bundleWithURL:url].executableURL)
        throw Error("application_not_found", "Application bundle is not installed. Use computer_list_apps.");
    struct Result { dispatch_semaphore_t done = dispatch_semaphore_create(0); __strong NSRunningApplication* app = nil; __strong NSError* error = nil; };
    auto result = std::make_shared<Result>();
    [NSWorkspace.sharedWorkspace openApplicationAtURL:url configuration:NSWorkspaceOpenConfiguration.configuration completionHandler:^(NSRunningApplication* app, NSError* error) {
        result->app = app; result->error = error; dispatch_semaphore_signal(result->done);
    }];
    if (dispatch_semaphore_wait(result->done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0)
        throw Error("launch_timeout", "Application launch has not completed. List windows before retrying.");
    if (!result->app) throw Error("launch_failed", text(result->error.localizedDescription));
    return {{"app", application}, {"pid", result->app.processIdentifier}, {"requires_observation", true}};
}
void activate(const Window& value) {
    require_accessibility(); require_interactive_session();
    NSRunningApplication* application = [NSRunningApplication runningApplicationWithProcessIdentifier:value.pid];
    if (!application || start_time(value.pid) != value.process_start) throw Error("stale_window", "Application was replaced.");
    [application activateWithOptions:0];
    if (value.root) ax_check(AXUIElementPerformAction(value.root.get(), kAXRaiseAction), "Raise window");
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    do {
        AX app(AXUIElementCreateApplication(value.pid));
        auto focused = ax_attribute(app.get(), kAXFocusedWindowAttribute);
        if (NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == value.pid &&
            focused && value.root && CFEqual(focused.get(), value.root.get())) return;
        usleep(20000);
    } while (Clock::now() < deadline);
    throw Error("activation_failed", "macOS did not focus the requested window. Activate it manually and observe again.");
}

std::vector<Surface> related_surfaces(const Window& root) {
    std::vector<Surface> result{{root}};
    const auto candidates = windows(false);
    std::vector<AX> pending;
    // Menus and popovers can be descendants outside the root rectangle. Search
    // a bounded AX hierarchy; never accept a same-PID window without AX lineage.
    pending.push_back(root.root);
    AX app(AXUIElementCreateApplication(root.pid));
    const auto focused_window = ax_attribute(app.get(), kAXFocusedWindowAttribute);
    if (focused_window && CFEqual(focused_window.get(), root.root.get()) &&
        NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == root.pid) {
        // The application menu belongs to its verified focused window. Follow
        // AX hierarchy to find open menus; a same-process CG window alone is
        // never ownership evidence for an unrelated floating panel.
        auto menu_bar = ax_attribute(app.get(), kAXMenuBarAttribute);
        if (menu_bar) pending.push_back(menu_bar);
        for (auto child : ax_children(app.get()))
            if (string_attribute(child.get(), kAXRoleAttribute, 64) == "AXMenu") pending.push_back(std::move(child));
    }
    std::vector<AX> seen;
    const auto deadline = Clock::now() + std::chrono::milliseconds(800);
    for (std::size_t i = 0; i < pending.size() && i < 160 && Clock::now() < deadline; ++i) {
        AX current = pending[i];
        if (std::any_of(seen.begin(), seen.end(), [&](const AX& item) { return CFEqual(item.get(), current.get()); })) continue;
        seen.push_back(current);
        auto role = string_attribute(current.get(), kAXRoleAttribute, 64);
        if (role == "AXSheet" || role == "AXMenu" || role == "AXPopover" || (role == "AXWindow" && !CFEqual(current.get(), root.root.get()))) {
            auto bounds = ax_bounds(current.get());
            std::vector<Window> matching;
            for (auto value : candidates) if (value.pid == root.pid && same_rect(bounds, value.rect) && value.id != root.id) matching.push_back(std::move(value));
            if (matching.size() == 1 && result.size() < 4 && std::none_of(result.begin(), result.end(), [&](const Surface& surface) { return surface.window.id == matching[0].id; })) {
                matching[0].root = current;
                result.push_back({matching[0], role});
            }
        }
        auto children = ax_children(current.get());
        for (auto& child : children) { if (pending.size() >= 256) break; pending.push_back(std::move(child)); }
    }
    return result;
}
void validate_surfaces(const Observation& observation) {
    for (const auto& surface : observation.surfaces) {
        const auto latest = window(surface.window.id, false);
        if (latest.pid != surface.window.pid || latest.process_start != surface.window.process_start ||
            !same_rect(latest.rect, surface.window.rect) || !same_rect(ax_bounds(surface.window.root.get()), surface.window.rect))
            throw Error("stale_window", "An observed window or popup changed. Observe again.");
    }
    auto current = related_surfaces(observation.surfaces.front().window);
    if (current.size() != observation.surfaces.size()) throw Error("stale_surface", "Related windows changed. Observe again.");
    for (std::size_t i = 0; i < current.size(); ++i)
        if (current[i].window.id != observation.surfaces[i].window.id || !CFEqual(current[i].window.root.get(), observation.surfaces[i].window.root.get()))
            throw Error("stale_surface", "Related windows were replaced. Observe again.");
}
void validate_hit(CGPoint point, const Surface& surface, AXUIElementRef element) {
    if (!CGRectContainsPoint(surface.window.rect, point)) throw Error("invalid_coordinate", "Target is outside the selected surface.");
    Ref<CFArrayRef> list(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
    bool found = false;
    for (NSDictionary* item in (__bridge NSArray*)list.get()) {
        const CGWindowID id = [item[(__bridge NSString*)kCGWindowNumber] unsignedIntValue];
        if (pointer_owns(id) || [item[(__bridge NSString*)kCGWindowAlpha] doubleValue] <= 0) continue;
        CGRect rect{};
        if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)item[(__bridge NSString*)kCGWindowBounds], &rect) || !CGRectContainsPoint(rect, point)) continue;
        // Dock's backing window can span the entire display, including its
        // transparent, input-pass-through region. Its rectangle cannot prove
        // occlusion. The system-wide AX hit test below still rejects Dock items.
        if ([item[(__bridge NSString*)kCGWindowLayer] integerValue] == CGWindowLevelForKey(kCGDockWindowLevelKey)) {
            auto owner = [NSRunningApplication runningApplicationWithProcessIdentifier:
                [item[(__bridge NSString*)kCGWindowOwnerPID] intValue]];
            if ([owner.bundleIdentifier isEqualToString:@"com.apple.dock"] &&
                [owner.bundleURL.path isEqualToString:@"/System/Library/CoreServices/Dock.app"]) continue;
        }
        if (id != surface.window.id) throw Error("target_obscured", "Window " + std::to_string(id) + " (" +
            text(item[(__bridge NSString*)kCGWindowOwnerName]) + ") covers this input point. Observe the active surface.");
        found = true; break;
    }
    if (!found) throw Error("target_unavailable", "Input target is not on the visible desktop.");
    AX system(AXUIElementCreateSystemWide());
    AXUIElementRef raw = nullptr;
    ax_check(AXUIElementCopyElementAtPosition(system.get(), point.x, point.y, &raw), "Hit-test input target");
    AX hit(raw);
    if (!hit || !ax_descendant(hit.get(), element ?: surface.window.root.get()))
        throw Error("target_obscured", "Accessibility hit target changed. Observe again.");
}
} // namespace acecode::computer_use::macos
