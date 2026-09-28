#include "web_host.hpp"

#include "application_icon.hpp"
#include "external_url.hpp"
#include "linux_webview_scale_policy.hpp"
#include "taskbar_badge_win.hpp"
#include "tray_icon_win.hpp"
#include "web_host_close_policy.hpp"
#include "webview2_runtime_probe.hpp"
#include "window_background.hpp"
#include "window_chrome.hpp"
#include "window_size.hpp"

#ifdef ACECODE_DEEPIN
#include "deepin_window_effects.hpp"
#endif

#include "utils/encoding.hpp"
#include "utils/logger.hpp"

#include <functional>
#include <limits>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include <webview/webview.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <wrl.h>  // Microsoft::WRL::Callback,挂 WebView2 事件 handler
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
#  include "linux_desktop.hpp"
#  include <dlfcn.h>
#  include <gtk/gtk.h>
#  include <webkit2/webkit2.h>
#  ifdef GDK_WINDOWING_X11
#    include <gdk/gdkx.h>
#  endif
#endif
#ifdef __APPLE__
#  include <CoreGraphics/CoreGraphics.h>
#  import <AppKit/AppKit.h>
#  include <objc/runtime.h>
#endif

// X11 arrives through WebKitGTK and defines these common C++ enum names as
// macros. Keep them from rewriting the clipboard result type below.
#ifdef Status
#  undef Status
#endif
#ifdef Success
#  undef Success
#endif

namespace acecode::desktop {

namespace {

// 全局 close_request_handler — 只在主线程上写,窗口回调在同一 GUI 主线程上读。
std::function<bool()> g_close_handler;

// 系统文件拖放 handler(plan: 桌面控制台拖放文件 → 插入完整路径)。Windows 的
// WebView2 事件回调 / macOS swizzle 的拖放回调命中文件时调它,把路径交给 main.cpp
// eval 回前端。主线程 only(WebView2 事件与 AppKit 拖放均在 GUI 主线程)。
WebHost::FileDropHandler g_file_drop_handler;

// A second Desktop process can carry more intent than "show the window".
// main.cpp uses this GUI-thread callback to consume its one-shot open request.
std::function<void()> g_existing_instance_focus_handler;

#ifdef __APPLE__
std::function<void(bool)> g_mac_window_state_handler;
bool g_mac_last_known_maximized = false;
std::function<void(bool)> g_mac_window_fullscreen_handler;
bool g_mac_last_known_fullscreen = false;
NSWindow* g_mac_reopen_window = nil;
webview::webview* g_mac_quit_webview = nullptr;
using MacApplicationReopenImp = BOOL (*)(id, SEL, NSApplication*, BOOL);
MacApplicationReopenImp g_mac_original_reopen_imp = nullptr;
NSString* const kAceCodeFocusExistingNotification =
    @"dev.acecode.desktop.focusExisting.v1";

NSWindow* mac_window_from_host(webview::webview& w) {
    auto window_result = w.window();
    if (!window_result.ok() || !window_result.value()) return nil;
    return static_cast<NSWindow*>(window_result.value());
}

void notify_mac_window_state_if_changed(NSWindow* window) {
    if (!window) return;
    const bool maximized = [window isZoomed] == YES;
    if (maximized == g_mac_last_known_maximized) return;
    g_mac_last_known_maximized = maximized;
    if (g_mac_window_state_handler) {
        g_mac_window_state_handler(maximized);
    }
}

bool mac_window_is_fullscreen(NSWindow* window) {
    return window &&
           (([window styleMask] & NSWindowStyleMaskFullScreen) != 0);
}

constexpr CGFloat kMacTopbarControlCenterFromTop = 20.0;

void show_and_align_mac_standard_button(NSWindow* window,
                                        NSWindowButton button,
                                        CGFloat horizontal_offset) {
    NSButton* button_view = [window standardWindowButton:button];
    if (!button_view) return;
    [button_view setHidden:NO];

    NSView* container = [button_view superview];
    if (!container) return;

    const NSRect bounds = [container bounds];
    NSRect frame = [button_view frame];
    const CGFloat target_center_y = [container isFlipped]
        ? NSMinY(bounds) + kMacTopbarControlCenterFromTop
        : NSMaxY(bounds) - kMacTopbarControlCenterFromTop;
    frame.origin.x += horizontal_offset;
    frame.origin.y = target_center_y - NSHeight(frame) / 2.0;
    [button_view setFrame:frame];
}

void align_mac_standard_buttons(NSWindow* window) {
    NSButton* close_button =
        [window standardWindowButton:NSWindowCloseButton];
    CGFloat horizontal_offset = 0.0;
    if (close_button && [close_button superview]) {
        const NSRect container_bounds = [[close_button superview] bounds];
        const NSRect close_frame = [close_button frame];
        const CGFloat equal_edge_inset =
            kMacTopbarControlCenterFromTop - NSHeight(close_frame) / 2.0;
        const CGFloat target_close_x =
            NSMinX(container_bounds) + equal_edge_inset;
        horizontal_offset = target_close_x - NSMinX(close_frame);
    }

    show_and_align_mac_standard_button(
        window, NSWindowCloseButton, horizontal_offset);
    show_and_align_mac_standard_button(
        window, NSWindowMiniaturizeButton, horizontal_offset);
    show_and_align_mac_standard_button(
        window, NSWindowZoomButton, horizontal_offset);
}

void notify_mac_window_fullscreen_if_changed(NSWindow* window) {
    if (!window) return;
    const bool fullscreen = mac_window_is_fullscreen(window);
    if (fullscreen == g_mac_last_known_fullscreen) return;
    g_mac_last_known_fullscreen = fullscreen;
    if (!fullscreen) {
        align_mac_standard_buttons(window);
    }
    if (g_mac_window_fullscreen_handler) {
        g_mac_window_fullscreen_handler(fullscreen);
    }
}

id install_mac_window_fullscreen_observer(webview::webview& w,
                                          NSNotificationName name) {
    NSWindow* window = mac_window_from_host(w);
    if (!window) return nil;
    return [[NSNotificationCenter defaultCenter]
        addObserverForName:name
                    object:window
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(__unused NSNotification* note) {
                    notify_mac_window_fullscreen_if_changed(window);
                }];
}

void configure_mac_window_chrome(webview::webview& w) {
    NSWindow* window = mac_window_from_host(w);
    if (!window) return;

    NSWindowStyleMask style = [window styleMask];
    style |= NSWindowStyleMaskTitled;
    style |= NSWindowStyleMaskClosable;
    style |= NSWindowStyleMaskMiniaturizable;
    style |= NSWindowStyleMaskResizable;
    style |= NSWindowStyleMaskFullSizeContentView;
    [window setStyleMask:style];
    [window setHasShadow:YES];
    [window setTitleVisibility:NSWindowTitleHidden];
    [window setTitlebarAppearsTransparent:YES];
    [window setToolbar:nil];
    [window setMovableByWindowBackground:NO];

    NSSize min_size = [window minSize];
    min_size.width = std::max(
        min_size.width,
        static_cast<CGFloat>(kMinimumDesktopWindowWidth));
    min_size.height = std::max(min_size.height, static_cast<CGFloat>(240.0));
    [window setMinSize:min_size];

    // Keep AppKit's hierarchy and actions intact, but align the native traffic
    // lights with the web top-bar controls and match their top/left edge insets.
    align_mac_standard_buttons(window);

    g_mac_last_known_maximized = [window isZoomed] == YES;
    g_mac_last_known_fullscreen = mac_window_is_fullscreen(window);
}

void show_mac_window(NSWindow* window) {
    if (!window) return;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    if ([window isMiniaturized]) {
        [window deminiaturize:nil];
    }
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

void hide_mac_window_to_tray(NSWindow* window) {
    if (!window) return;
    [window orderOut:nil];
    // Keep the regular activation policy so the Dock icon remains available as
    // a restore affordance. Window close is not application quit.
}

BOOL acecode_application_should_handle_reopen(
    id self,
    SEL command,
    NSApplication* application,
    BOOL has_visible_windows) {
    if (g_mac_reopen_window &&
        (![g_mac_reopen_window isVisible] ||
         [g_mac_reopen_window isMiniaturized])) {
        show_mac_window(g_mac_reopen_window);
    }
    if (g_mac_original_reopen_imp) {
        return g_mac_original_reopen_imp(
            self, command, application, has_visible_windows);
    }
    return YES;
}

void install_mac_application_reopen_handler(NSWindow* window) {
    if (!window) return;
    g_mac_reopen_window = window;
    id delegate = [NSApp delegate];
    if (!delegate) return;
    Class cls = object_getClass(delegate);
    if (!cls) return;
    const SEL selector =
        @selector(applicationShouldHandleReopen:hasVisibleWindows:);
    if (class_addMethod(
            cls,
            selector,
            reinterpret_cast<IMP>(
                acecode_application_should_handle_reopen),
            "c@:@c")) {
        return;
    }
    Method method = class_getInstanceMethod(cls, selector);
    if (!method) return;
    IMP current = method_getImplementation(method);
    if (current == reinterpret_cast<IMP>(
            acecode_application_should_handle_reopen)) {
        return;
    }
    g_mac_original_reopen_imp =
        reinterpret_cast<MacApplicationReopenImp>(current);
    method_setImplementation(
        method,
        reinterpret_cast<IMP>(
            acecode_application_should_handle_reopen));
}

void acecode_mac_request_quit(
    id /*application*/,
    SEL /*command*/,
    id sender) {
    if (g_mac_quit_webview) {
        g_mac_quit_webview->terminate();
        return;
    }
    [NSApp terminate:sender];
}

id install_mac_focus_existing_observer(webview::webview& w) {
    NSWindow* window = mac_window_from_host(w);
    if (!window) return nil;
    return [[NSDistributedNotificationCenter defaultCenter]
        addObserverForName:kAceCodeFocusExistingNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(__unused NSNotification* note) {
                    show_mac_window(window);
                    if (g_existing_instance_focus_handler) {
                        g_existing_instance_focus_handler();
                    }
                }];
}

NSEvent* mac_synthetic_left_mouse_event(NSWindow* window) {
    if (!window) return nil;
    return [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                              location:[window mouseLocationOutsideOfEventStream]
                         modifierFlags:0
                             timestamp:[[NSProcessInfo processInfo] systemUptime]
                          windowNumber:[window windowNumber]
                               context:nil
                           eventNumber:0
                            clickCount:1
                              pressure:1.0];
}

bool mac_perform_window_drag(NSWindow* window) {
    if (!window) return false;
    NSEvent* event = mac_synthetic_left_mouse_event(window);
    if (!event) return false;
    [window performWindowDragWithEvent:event];
    notify_mac_window_state_if_changed(window);
    return true;
}

bool mac_area_uses_left_edge(FramelessHitTestArea area) {
    return area == FramelessHitTestArea::Left ||
           area == FramelessHitTestArea::TopLeft ||
           area == FramelessHitTestArea::BottomLeft;
}

bool mac_area_uses_right_edge(FramelessHitTestArea area) {
    return area == FramelessHitTestArea::Right ||
           area == FramelessHitTestArea::TopRight ||
           area == FramelessHitTestArea::BottomRight;
}

bool mac_area_uses_top_edge(FramelessHitTestArea area) {
    return area == FramelessHitTestArea::Top ||
           area == FramelessHitTestArea::TopLeft ||
           area == FramelessHitTestArea::TopRight;
}

bool mac_area_uses_bottom_edge(FramelessHitTestArea area) {
    return area == FramelessHitTestArea::Bottom ||
           area == FramelessHitTestArea::BottomLeft ||
           area == FramelessHitTestArea::BottomRight;
}

NSSize mac_min_window_size(NSWindow* window) {
    NSSize min_size = window ? [window minSize] : NSMakeSize(0, 0);
    min_size.width = std::max(
        min_size.width,
        static_cast<CGFloat>(kMinimumDesktopWindowWidth));
    min_size.height = std::max(min_size.height, static_cast<CGFloat>(240.0));
    return min_size;
}

NSRect mac_resized_frame(NSRect start_frame,
                         NSPoint start_mouse,
                         NSPoint current_mouse,
                         NSSize min_size,
                         FramelessHitTestArea area) {
    const CGFloat dx = current_mouse.x - start_mouse.x;
    const CGFloat dy = current_mouse.y - start_mouse.y;
    const CGFloat min_width = std::max(min_size.width, static_cast<CGFloat>(1.0));
    const CGFloat min_height = std::max(min_size.height, static_cast<CGFloat>(1.0));
    const CGFloat start_right = NSMaxX(start_frame);
    const CGFloat start_top = NSMaxY(start_frame);

    NSRect next = start_frame;
    if (mac_area_uses_left_edge(area)) {
        next.origin.x = start_frame.origin.x + dx;
        next.size.width = start_frame.size.width - dx;
        if (next.size.width < min_width) {
            next.size.width = min_width;
            next.origin.x = start_right - min_width;
        }
    } else if (mac_area_uses_right_edge(area)) {
        next.size.width = std::max(min_width, start_frame.size.width + dx);
    }

    if (mac_area_uses_bottom_edge(area)) {
        next.origin.y = start_frame.origin.y + dy;
        next.size.height = start_frame.size.height - dy;
        if (next.size.height < min_height) {
            next.size.height = min_height;
            next.origin.y = start_top - min_height;
        }
    } else if (mac_area_uses_top_edge(area)) {
        next.size.height = std::max(min_height, start_frame.size.height + dy);
    }
    return next;
}

bool mac_track_window_resize(NSWindow* window, FramelessHitTestArea area) {
    if (!window || [window isZoomed]) return false;
    if (!mac_area_uses_left_edge(area) && !mac_area_uses_right_edge(area) &&
        !mac_area_uses_top_edge(area) && !mac_area_uses_bottom_edge(area)) {
        return false;
    }

    const NSRect start_frame = [window frame];
    const NSPoint start_mouse = [NSEvent mouseLocation];
    const NSSize min_size = mac_min_window_size(window);
    const NSEventMask mask = NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp;

    while (([NSEvent pressedMouseButtons] & 1) != 0) {
        @autoreleasepool {
            NSEvent* event = [NSApp nextEventMatchingMask:mask
                                                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]
                                                   inMode:NSEventTrackingRunLoopMode
                                                  dequeue:YES];
            if (!event) continue;
            const NSEventType type = [event type];
            if (type == NSEventTypeLeftMouseUp) break;
            if (type != NSEventTypeLeftMouseDragged) continue;

            const NSRect next = mac_resized_frame(start_frame,
                                                  start_mouse,
                                                  [NSEvent mouseLocation],
                                                  min_size,
                                                  area);
            [window setFrame:next display:YES animate:NO];
        }
    }
    notify_mac_window_state_if_changed(window);
    return true;
}

BOOL acecode_mac_window_should_close(id /*self*/, SEL /*cmd*/, id /*sender*/) {
    return dispatch_wm_close(g_close_handler) == CloseDispatch::ConsumedByHandler
        ? NO
        : YES;
}

void install_mac_close_handler(webview::webview& w) {
    NSWindow* window = mac_window_from_host(w);
    if (!window) return;
    id delegate = [window delegate];
    if (!delegate) return;
    Class cls = object_getClass(delegate);
    if (!cls) return;
    class_addMethod(cls,
                    @selector(windowShouldClose:),
                    reinterpret_cast<IMP>(acecode_mac_window_should_close),
                    "c@:@");
}

// macOS WKWebView 不自动转发 Cmd+C/V/X/A 等标准快捷键。
// 设置包含 Edit 菜单的 NSMenu 后,系统会通过 responder chain
// 将这些 keyEquivalent 路由到 WKWebView,从而使快捷键生效。
void install_mac_edit_menu(webview::webview& w) {
    g_mac_quit_webview = &w;
    const SEL quit_selector = NSSelectorFromString(@"acecodeRequestQuit:");
    class_addMethod(
        [NSApplication class],
        quit_selector,
        reinterpret_cast<IMP>(acecode_mac_request_quit),
        "v@:@");

    NSMenu* main_menu = [[NSMenu alloc] init];

    // Application menu (macOS 要求第一个菜单项为应用菜单)
    NSMenuItem* app_menu_item = [[NSMenuItem alloc] init];
    NSMenu* app_menu = [[NSMenu alloc] init];
    NSMenuItem* quit_item =
        [app_menu addItemWithTitle:@"Quit ACECode"
                            action:quit_selector
                     keyEquivalent:@"q"];
    [quit_item setTarget:NSApp];
    [app_menu_item setSubmenu:app_menu];
    [main_menu addItem:app_menu_item];

    // Edit menu — 提供给 WKWebView 响应 Cmd+C/V/X/A/Z
    NSMenuItem* edit_menu_item = [[NSMenuItem alloc] init];
    NSMenu* edit_menu = [[NSMenu alloc] initWithTitle:@"Edit"];

    [edit_menu addItemWithTitle:@"Undo"
                         action:@selector(undo:)
                  keyEquivalent:@"z"];
    NSMenuItem* redo_item = [edit_menu addItemWithTitle:@"Redo"
                                                 action:@selector(redo:)
                                          keyEquivalent:@"z"];
    [redo_item setKeyEquivalentModifierMask:NSEventModifierFlagCommand |
                                            NSEventModifierFlagShift];
    [edit_menu addItem:[NSMenuItem separatorItem]];
    [edit_menu addItemWithTitle:@"Cut"
                         action:@selector(cut:)
                  keyEquivalent:@"x"];
    [edit_menu addItemWithTitle:@"Copy"
                         action:@selector(copy:)
                  keyEquivalent:@"c"];
    [edit_menu addItemWithTitle:@"Paste"
                         action:@selector(paste:)
                  keyEquivalent:@"v"];
    [edit_menu addItemWithTitle:@"Select All"
                         action:@selector(selectAll:)
                  keyEquivalent:@"a"];

    [edit_menu_item setSubmenu:edit_menu];
    [main_menu addItem:edit_menu_item];

    [NSApp setMainMenu:main_menu];
}

// ── macOS 系统文件拖放接管 ─────────────────────────────────────────────
// WKWebView 默认把 Finder 文件拖放当网页内拖放/导航处理,JS 拿不到完整路径。
// 这里 swizzle WKWebView 类的拖放方法:pasteboard 含 fileURL(Finder 拖来)→
// 取出本地路径并回传;否则调原实现,不破坏网页内拖放(如把图片拖进编辑区)。
typedef BOOL (*PerformDragImp)(id, SEL, id);
typedef NSDragOperation (*DragOpImp)(id, SEL, id);
static PerformDragImp g_orig_perform_drag = nullptr;
static DragOpImp g_orig_dragging_entered = nullptr;
static DragOpImp g_orig_dragging_updated = nullptr;

static bool mac_drag_has_file_urls(id sender) {
    NSPasteboard* pb = [sender draggingPasteboard];
    return [pb canReadObjectForClasses:@[ [NSURL class] ]
                               options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
}

static std::optional<WebHost::FileDropLocation> mac_drop_location(id self, id sender) {
    NSView* view = [self isKindOfClass:[NSView class]] ? static_cast<NSView*>(self) : nil;
    if (!view || !sender) return std::nullopt;
    const NSRect bounds = [view bounds];
    if (bounds.size.width <= 0.0 || bounds.size.height <= 0.0) return std::nullopt;

    const NSPoint window_point = [sender draggingLocation];
    const NSPoint local_point = [view convertPoint:window_point fromView:nil];
    const double x = (local_point.x - NSMinX(bounds)) / NSWidth(bounds);
    const double local_y = (local_point.y - NSMinY(bounds)) / NSHeight(bounds);
    const double y = [view isFlipped] ? local_y : 1.0 - local_y;
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x >= 1.0 ||
        y < 0.0 || y >= 1.0) {
        return std::nullopt;
    }
    return WebHost::FileDropLocation{x, y};
}

static BOOL ace_perform_drag_operation(id self, SEL cmd, id sender) {
    const bool has_file_urls = mac_drag_has_file_urls(sender);
    if (has_file_urls) {
        NSArray<NSURL*>* urls = [[sender draggingPasteboard]
            readObjectsForClasses:@[ [NSURL class] ]
                          options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
        std::vector<std::string> paths;
        for (NSURL* url in urls) {
            if (url.path) {
                const char* p = [url.path UTF8String];
                if (p) paths.emplace_back(p);
            }
        }
        const auto location = mac_drop_location(self, sender);
        if (!paths.empty() && g_file_drop_handler) {
            if (!location) {
                LOG_WARN("[file-drop] macOS drop rejected count=" +
                         std::to_string(paths.size()) + " reason=invalid-coordinate");
            }
            g_file_drop_handler(paths, WebHost::FileDropContext{location, true});
            return YES;
        }
        LOG_WARN("[file-drop] macOS drop rejected count=" +
                 std::to_string(paths.size()) +
                 " coordinate_valid=" + (location ? "true" : "false") +
                 " handler=" + (g_file_drop_handler ? "available" : "missing"));
    }
    if (g_orig_perform_drag) return g_orig_perform_drag(self, cmd, sender);
    return NO;
}

static NSDragOperation ace_dragging_entered(id self, SEL cmd, id sender) {
    if (mac_drag_has_file_urls(sender)) return NSDragOperationCopy;
    if (g_orig_dragging_entered) return g_orig_dragging_entered(self, cmd, sender);
    return NSDragOperationNone;
}

static NSDragOperation ace_dragging_updated(id self, SEL cmd, id sender) {
    if (mac_drag_has_file_urls(sender)) return NSDragOperationCopy;
    if (g_orig_dragging_updated) return g_orig_dragging_updated(self, cmd, sender);
    return NSDragOperationNone;
}

void install_mac_file_drop(webview::webview& w) {
    auto widget_result = w.widget();
    if (!widget_result.ok() || !widget_result.value()) return;
    NSView* view = static_cast<NSView*>(widget_result.value());
    if (![view isKindOfClass:[NSView class]]) return;
    Class cls = object_getClass(view);  // WKWebView 实际类
    if (!cls) return;
    LOG_INFO("[file-drop] macOS installation class=" +
             std::string(class_getName(cls)) +
             " handler=" + (g_file_drop_handler ? "available" : "missing"));

    // 补注册 fileURL 拖放类型(WKWebView 已注册网页拖放类型,合并而非覆盖)。
    @try {
        NSMutableArray* types = [[view registeredDraggedTypes] mutableCopy];
        if (!types) types = [NSMutableArray array];
        if (![types containsObject:NSPasteboardTypeFileURL]) {
            [types addObject:NSPasteboardTypeFileURL];
            [view registerForDraggedTypes:types];
        }
    } @catch (...) {
    }

    auto swizzle = [&](SEL sel, IMP repl, const char* types, IMP* saved_orig) {
        if (Method m = class_getInstanceMethod(cls, sel)) {
            if (saved_orig) *saved_orig = method_getImplementation(m);
            method_setImplementation(m, repl);
        } else {
            class_addMethod(cls, sel, repl, types);
        }
    };
    swizzle(@selector(performDragOperation:), (IMP)ace_perform_drag_operation, "c@:@",
            reinterpret_cast<IMP*>(&g_orig_perform_drag));
    swizzle(@selector(draggingEntered:), (IMP)ace_dragging_entered, "Q@:@",
            reinterpret_cast<IMP*>(&g_orig_dragging_entered));
    swizzle(@selector(draggingUpdated:), (IMP)ace_dragging_updated, "Q@:@",
            reinterpret_cast<IMP*>(&g_orig_dragging_updated));
}
#endif

} // namespace

#ifdef _WIN32
namespace {

constexpr wchar_t kHostWindowClassName[] = L"ACECodeDesktopHostWindow";
constexpr wchar_t kHostWindowPreviousProcProperty[] = L"ACECodeDesktopHostPreviousProc";
constexpr wchar_t kHostWindowStartupMonitorProperty[] = L"ACECodeDesktopStartupMonitor";
constexpr int kFramelessDragHeightDip = 44;

HICON load_host_window_icon(int width, int height) {
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    // acecode.rc.in uses numeric ID 1; older resources used the name IDI_ICON1.
    if (HICON icon = static_cast<HICON>(::LoadImageW(
            instance, MAKEINTRESOURCEW(1), IMAGE_ICON, width, height,
            LR_DEFAULTCOLOR))) {
        return icon;
    }
    if (HICON icon = static_cast<HICON>(::LoadImageW(
            instance, L"IDI_ICON1", IMAGE_ICON, width, height, LR_DEFAULTCOLOR))) {
        return icon;
    }
    LOG_WARN("[desktop] failed to load window icon, last_error=" +
             std::to_string(::GetLastError()));
    return nullptr;
}

struct HostWindowIcons {
    // Own distinct sizes: LR_SHARED can return a cached frame of the wrong size.
    HICON large_icon = load_host_window_icon(
        ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON));
    HICON small_icon = load_host_window_icon(
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON));

    ~HostWindowIcons() {
        if (large_icon) ::DestroyIcon(large_icon);
        if (small_icon) ::DestroyIcon(small_icon);
    }
};

const HostWindowIcons& host_window_icons() {
    // Window classes and all host windows share these until process shutdown.
    static const HostWindowIcons icons;
    return icons;
}

void apply_host_window_icons(HWND hwnd) {
    if (!hwnd) return;
    const auto& icons = host_window_icons();
    if (icons.large_icon) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_BIG,
                       reinterpret_cast<LPARAM>(icons.large_icon));
    }
    if (icons.small_icon) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                       reinterpret_cast<LPARAM>(icons.small_icon));
    }
}

// WM_USER 区私有消息:绕过 close_request_handler 直接走 DestroyWindow。
// 选 0x10 偏移留出 0..0xF 给未来扩展;远离 webview/Common Controls 常用的
// WM_USER..WM_USER+0x100 区段。
constexpr UINT kRequestQuitMsg = WM_USER + 0x10;

// 窗口最大化/还原状态变化 handler。同样 main thread only。WndProc 在 WM_SIZE
// 时检测 IsZoomed 与 g_last_known_maximized 是否不同,变化时触发并同步缓存。
// 缓存初始 false,WM_SIZE 第一次到达时若为 maximized 会被推送一次,frontend
// 也照样会通过 aceDesktop_isWindowMaximized 拿初始态;两者最终一致。
std::function<void(bool)> g_window_state_handler;
bool g_last_known_maximized = false;
std::function<void(bool)> g_window_visibility_handler;
bool g_last_known_window_visible = true;

void notify_window_visibility(bool visible) {
    if (visible == g_last_known_window_visible) return;
    g_last_known_window_visible = visible;
    if (g_window_visibility_handler) g_window_visibility_handler(visible);
}

// WebView2 does not reliably turn a native top-level activation into a DOM
// window.focus event. The Desktop main process uses this callback to dispatch
// an explicit frontend event after the host has completed WM_ACTIVATE handling.
std::function<void()> g_window_focus_handler;

// 单例联动:第二次启动 acecode-desktop 会向已有 host window 派 focus msg,
// 让我们把窗口拉前 + 显示。同名 RegisterWindowMessageW 在两端拿到一致 UINT,
// 见 single_instance_win.cpp 头注。第一次访问时 lazily register,缓存到 static。
UINT focus_existing_instance_msg() {
    static UINT id = ::RegisterWindowMessageW(L"ACECode_FocusExistingInstance_v1");
    return id;
}

WNDPROC previous_host_window_proc(HWND hwnd) {
    return reinterpret_cast<WNDPROC>(::GetPropW(hwnd, kHostWindowPreviousProcProperty));
}

LRESULT call_host_default_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (WNDPROC previous = previous_host_window_proc(hwnd)) {
        return ::CallWindowProcW(previous, hwnd, msg, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

int dpi_scale(int value, UINT dpi) {
    return static_cast<int>((static_cast<long long>(value) * static_cast<long long>(dpi)) / 96);
}

UINT monitor_dpi(HMONITOR monitor) {
    if (monitor) {
        if (HMODULE shcore = ::LoadLibraryW(L"Shcore.dll")) {
            using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
            auto get_dpi = reinterpret_cast<GetDpiForMonitorFn>(
                ::GetProcAddress(shcore, "GetDpiForMonitor"));
            UINT dpi_x = 96;
            UINT dpi_y = 96;
            if (get_dpi && SUCCEEDED(get_dpi(monitor, 0, &dpi_x, &dpi_y)) &&
                dpi_x > 0) {
                ::FreeLibrary(shcore);
                return dpi_x;
            }
            ::FreeLibrary(shcore);
        }
    }

    HDC hdc = ::GetDC(nullptr);
    const int fallback_dpi = hdc ? ::GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc) ::ReleaseDC(nullptr, hdc);
    return fallback_dpi > 0 ? static_cast<UINT>(fallback_dpi) : 96U;
}

RECT monitor_work_rect(HMONITOR monitor) {
    RECT fallback{
        0,
        0,
        ::GetSystemMetrics(SM_CXSCREEN),
        ::GetSystemMetrics(SM_CYSCREEN),
    };
    if (!monitor) return fallback;

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info)) return fallback;
    return info.rcWork;
}

void apply_minimum_track_size(HWND hwnd, MINMAXINFO* info) {
    if (!info) return;
    HMONITOR monitor = reinterpret_cast<HMONITOR>(
        ::GetPropW(hwnd, kHostWindowStartupMonitorProperty));
    if (!monitor) {
        monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    }
    UINT dpi = monitor_dpi(monitor);
    if (dpi == 0) {
        dpi = ::GetDpiForWindow(hwnd);
    }
    if (dpi == 0) dpi = 96;

    LONG preferred_width = static_cast<LONG>(
        dpi_scale(kMinimumDesktopWindowWidth, dpi));
    LONG maximum_width = std::numeric_limits<LONG>::max();
    if (monitor) {
        const RECT work_area = monitor_work_rect(monitor);
        const auto safe_default = fit_desktop_window_to_safe_work_area(
            {kDefaultDesktopWindowWidth, kDefaultDesktopWindowHeight},
            {work_area.right - work_area.left, work_area.bottom - work_area.top},
            static_cast<int>(dpi));
        maximum_width = std::max<LONG>(1, safe_default.width);
        preferred_width = std::min(preferred_width, maximum_width);
    }
    info->ptMinTrackSize.x = std::min(
        maximum_width,
        std::max<LONG>(info->ptMinTrackSize.x, std::max<LONG>(1, preferred_width)));
}

HMONITOR active_monitor() {
    if (HWND fg = ::GetForegroundWindow()) {
        if (HMONITOR monitor = ::MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST)) {
            return monitor;
        }
    }
    POINT pt{};
    if (::GetCursorPos(&pt)) {
        if (HMONITOR monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)) {
            return monitor;
        }
    }
    return ::MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
}

// ── Win10 顶边 1px 边框线 ─────────────────────────────────────────────
// 系统在 Win10 上不画无标题栏窗口的顶边线(原因见 window_chrome.hpp)。非最大化
// 时把主 WebView 下移 inset 像素,宿主窗口在露出的这一行按系统配色自己画线。
// 不走 DwmExtendFrameIntoClientArea 让系统画:Win10 上那样激活时四边边框都会
// 变白(microsoft/terminal#4577)。不画在网页里:页面拿不到激活态 / 主题色,
// 非整数缩放下 CSS 1px 也不等于 1 个物理像素(VS Code 因此在 Windows 上停用了
// CSS 窗口边框)。这一行归宿主窗口,frameless_hit_test 会给它原生 HTTOP。

// 宿主窗口按激活态画线;由 WM_NCACTIVATE 维护,与系统画左/右/下边框同源。
bool g_top_border_active = false;
// 线下方的标题栏底色 = 前端推来的 --ace-bg(apply_window_background 同步)。
WindowBackgroundColor g_top_border_background = kDefaultWindowBackground;

std::uint32_t windows_build_number() {
    static const std::uint32_t build = [] {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return std::uint32_t{0};
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        auto get_version = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (!get_version || get_version(&info) != 0) return std::uint32_t{0};
        return static_cast<std::uint32_t>(info.dwBuildNumber);
    }();
    return build;
}

int top_border_inset(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) return 0;
    SelfDrawnTopBorderLayoutInput input;
    input.supported = windows_needs_self_drawn_top_border(windows_build_number());
    if (!input.supported) return 0;
    input.has_resize_frame =
        (::GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) != 0;
    input.maximized = ::IsZoomed(hwnd) != FALSE;
    input.minimized = ::IsIconic(hwnd) != FALSE;
    input.system_dpi = static_cast<int>(::GetDpiForSystem());
    return self_drawn_top_border_inset(input);
}

std::optional<std::uint32_t> read_dwm_dword(const wchar_t* name) {
    DWORD value = 0;
    DWORD value_size = sizeof(value);
    const LONG result = ::RegGetValueW(HKEY_CURRENT_USER,
                                       L"Software\\Microsoft\\Windows\\DWM",
                                       name,
                                       RRF_RT_REG_DWORD,
                                       nullptr,
                                       &value,
                                       &value_size);
    if (result != ERROR_SUCCESS || value_size != sizeof(value)) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

// 每次绘制现读注册表:只在这 1px 进入重绘区时才走到,开销可忽略;用户改了
// 主题色设置但没有广播到本窗口时,下一次重绘也能自行纠正。
COLORREF top_border_colorref() {
    SelfDrawnTopBorderColorInput input;
    input.active = g_top_border_active;
    input.windows_build = windows_build_number();
    const auto prevalence = read_dwm_dword(L"ColorPrevalence");
    input.accent_on_borders = prevalence && *prevalence == 1;
    if (input.active && input.accent_on_borders) {
        input.colorization_color = read_dwm_dword(L"ColorizationColor");
        input.colorization_balance = read_dwm_dword(L"ColorizationColorBalance");
    }
    input.background = RgbColor{g_top_border_background.r,
                                g_top_border_background.g,
                                g_top_border_background.b};
    const RgbColor color = self_drawn_top_border_color(input);
    return RGB(color.r, color.g, color.b);
}

void invalidate_top_border(HWND hwnd) {
    const int inset = top_border_inset(hwnd);
    if (inset <= 0) return;
    RECT client{};
    if (!::GetClientRect(hwnd, &client)) return;
    const RECT strip{0, 0, client.right, inset};
    ::InvalidateRect(hwnd, &strip, FALSE);
}

// 返回 false 表示当前不画线(Win11 / 最大化等),调用方走默认 WM_PAINT。
bool paint_top_border(HWND hwnd) {
    const int inset = top_border_inset(hwnd);
    if (inset <= 0) return false;
    PAINTSTRUCT paint{};
    HDC dc = ::BeginPaint(hwnd, &paint);
    if (dc) {
        RECT client{};
        ::GetClientRect(hwnd, &client);
        const RECT strip{0, 0, client.right, inset};
        RECT dirty{};
        if (::IntersectRect(&dirty, &strip, &paint.rcPaint)) {
            if (HBRUSH brush = ::CreateSolidBrush(top_border_colorref())) {
                ::FillRect(dc, &dirty, brush);
                ::DeleteObject(brush);
            }
        }
    }
    ::EndPaint(hwnd, &paint);
    return true;
}

void resize_webview_widget(HWND hwnd) {
    HWND widget = ::FindWindowExW(hwnd, nullptr, kWebViewWidgetClassName, nullptr);
    if (!widget) return;

    RECT client{};
    if (!::GetClientRect(hwnd, &client)) return;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int inset = std::clamp(top_border_inset(hwnd), 0, std::max(0, height));
    ::MoveWindow(widget, 0, inset, width, height - inset, TRUE);
    if (inset > 0) invalidate_top_border(hwnd);
}

void refresh_non_client_frame(HWND hwnd) {
    RECT rect{};
    if (!::GetWindowRect(hwnd, &rect)) return;
    ::SetWindowPos(hwnd,
                   nullptr,
                   rect.left,
                   rect.top,
                   rect.right - rect.left,
                   rect.bottom - rect.top,
                   SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE |
                       SWP_NOSIZE);
}

void log_webview_setting_failure(const char* operation, HRESULT hr) {
    LOG_WARN(std::string("[desktop] ") + operation +
             " failed, hr=" + std::to_string(static_cast<long>(hr)));
}

bool set_dev_tools_enabled(ICoreWebView2* core, BOOL enabled) {
    if (!core) return false;
    ICoreWebView2Settings* settings = nullptr;
    HRESULT hr = core->get_Settings(&settings);
    if (FAILED(hr) || !settings) {
        log_webview_setting_failure("get_Settings for DevTools", hr);
        return false;
    }
    hr = settings->put_AreDevToolsEnabled(enabled);
    settings->Release();
    if (FAILED(hr)) {
        log_webview_setting_failure("put_AreDevToolsEnabled", hr);
        return false;
    }
    return true;
}

bool open_dev_tools_for_core(ICoreWebView2* core) {
    if (!core) return false;
    // webview/webview disables DevTools when constructed with debug=false.
    // Re-enable it only for ACECode's explicit F11/bridge entry point.
    if (!set_dev_tools_enabled(core, TRUE)) return false;
    HRESULT hr = core->OpenDevToolsWindow();
    if (FAILED(hr)) {
        log_webview_setting_failure("OpenDevToolsWindow", hr);
        return false;
    }
    return true;
}

void install_dev_tools_shortcut(ICoreWebView2Controller* controller) {
    if (!controller) return;
    using Microsoft::WRL::Callback;
    EventRegistrationToken token{};
    HRESULT hr = controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [](ICoreWebView2Controller* sender,
               ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                if (!sender || !args) return S_OK;

                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                UINT virtual_key = 0;
                if (FAILED(args->get_KeyEventKind(&kind)) ||
                    FAILED(args->get_VirtualKey(&virtual_key))) {
                    return S_OK;
                }
                const bool key_down =
                    kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                    kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
                if (!key_down || virtual_key != VK_F11) return S_OK;

                args->put_Handled(TRUE);
                ICoreWebView2* core = nullptr;
                if (SUCCEEDED(sender->get_CoreWebView2(&core)) && core) {
                    open_dev_tools_for_core(core);
                    core->Release();
                }
                return S_OK;
            })
            .Get(),
        &token);
    if (FAILED(hr)) {
        log_webview_setting_failure("add_AcceleratorKeyPressed for F11 DevTools", hr);
    }
}

void configure_browser_defaults(webview::webview& host) {
    auto controller_result = host.browser_controller();
    if (!controller_result.ok()) {
        LOG_WARN("[desktop] browser_controller unavailable; browser defaults not configured");
        return;
    }
    auto* controller = static_cast<ICoreWebView2Controller*>(controller_result.value());
    if (!controller) return;

    HRESULT hr = controller->put_ZoomFactor(1.0);
    if (FAILED(hr)) {
        log_webview_setting_failure("put_ZoomFactor", hr);
    }

    ICoreWebView2* core = nullptr;
    hr = controller->get_CoreWebView2(&core);
    if (FAILED(hr) || !core) {
        log_webview_setting_failure("get_CoreWebView2", hr);
        return;
    }
    install_dev_tools_shortcut(controller);

    ICoreWebView2Settings* settings = nullptr;
    hr = core->get_Settings(&settings);
    core->Release();
    if (FAILED(hr) || !settings) {
        log_webview_setting_failure("get_Settings", hr);
        return;
    }

    hr = settings->put_IsZoomControlEnabled(FALSE);
    if (FAILED(hr)) {
        log_webview_setting_failure("put_IsZoomControlEnabled", hr);
    }

    ICoreWebView2Settings5* settings5 = nullptr;
    hr = settings->QueryInterface(IID_PPV_ARGS(&settings5));
    if (SUCCEEDED(hr) && settings5) {
        const HRESULT pinch_hr = settings5->put_IsPinchZoomEnabled(FALSE);
        if (FAILED(pinch_hr)) {
            log_webview_setting_failure("put_IsPinchZoomEnabled", pinch_hr);
        }
        settings5->Release();
    }

    ICoreWebView2Settings6* settings6 = nullptr;
    hr = settings->QueryInterface(IID_PPV_ARGS(&settings6));
    if (SUCCEEDED(hr) && settings6) {
        const HRESULT swipe_hr = settings6->put_IsSwipeNavigationEnabled(FALSE);
        if (FAILED(swipe_hr)) {
            log_webview_setting_failure("put_IsSwipeNavigationEnabled", swipe_hr);
        }
        settings6->Release();
    }

    settings->Release();
}

// ── 窗口打底色(快速 resize 防黑边)────────────────────────────────────
// 窗口树三层(host HWND → webview_widget → WebView2 合成器)没有任何一层
// 自带背景 — 两个窗口类的 hbrBackground 都是 NULL,放大窗口时新暴露区域
// 不被擦除,肉眼看到黑闪;WebView2 自己的 DefaultBackgroundColor 默认白,
// 暗色主题下则闪白。修法与 Electron/Tauri 同构:三层涂同一个颜色,启动
// 默认取前端浅色 body 底色(kDefaultWindowBackground = #f5f5f2),前端
// ThemeProvider 再按实际主题经 aceDesktop_setWindowBackgroundColor 推送。

COLORREF background_colorref(WindowBackgroundColor color) {
    return RGB(color.r, color.g, color.b);
}

// 换掉窗口**类**的背景刷。类刷子由 DefWindowProc 在 WM_ERASEBKGND 时使用,
// 恰好覆盖 webview 库注册的 webview_widget / webview 类(它们的 wndproc 把
// 未处理消息都交给 DefWindowProc,库源码零改动)。旧刷子恒为 CreateSolidBrush
// 产物(或 NULL),DeleteObject 安全 — 本文件从不使用 COLOR_* 系统色伪句柄。
void apply_class_background_brush(HWND hwnd, WindowBackgroundColor color) {
    if (!hwnd || !::IsWindow(hwnd)) return;
    HBRUSH brush = ::CreateSolidBrush(background_colorref(color));
    if (!brush) return;
    ::SetLastError(ERROR_SUCCESS);
    LONG_PTR previous = ::SetClassLongPtrW(
        hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(brush));
    if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
        // 类刷子没换上,把新刷子收回,避免 GDI 句柄泄漏。
        ::DeleteObject(brush);
        return;
    }
    if (previous != 0) {
        ::DeleteObject(reinterpret_cast<HBRUSH>(previous));
    }
}

// WebView2 合成器自己的打底色(页面未渲染区域,如导航间隙 / resize 追帧)。
// 启动首帧之前的窗口由 WEBVIEW2_DEFAULT_BACKGROUND_COLOR 环境变量负责
// (Impl 构造体首,官方文档:仅属性设置在生效前仍会闪默认白);运行时主题
// 切换走这里的属性更新。
void apply_webview2_default_background(webview::webview& host,
                                       WindowBackgroundColor color) {
    auto controller_result = host.browser_controller();
    if (!controller_result.ok()) return;
    auto* controller = static_cast<ICoreWebView2Controller*>(controller_result.value());
    if (!controller) return;
    ICoreWebView2Controller2* controller2 = nullptr;
    HRESULT hr = controller->QueryInterface(IID_PPV_ARGS(&controller2));
    if (FAILED(hr) || !controller2) {
        log_webview_setting_failure("QueryInterface ICoreWebView2Controller2", hr);
        return;
    }
    COREWEBVIEW2_COLOR background{255, color.r, color.g, color.b};
    hr = controller2->put_DefaultBackgroundColor(background);
    controller2->Release();
    if (FAILED(hr)) {
        log_webview_setting_failure("put_DefaultBackgroundColor", hr);
    }
}

// 三层统一入口:host 窗口类 + webview_widget 类 + WebView2 合成器。
// host_hwnd 在 offscreen 路径是我们的 ACECodeDesktopHostWindow 类(注册时
// 已带默认刷,这里换成等价新刷无害),在降级路径是 webview 库自建的
// `webview` 类(NULL 刷,必须靠这里补上)。GUI 主线程 only。
void apply_window_background(webview::webview& host,
                             HWND host_hwnd,
                             WindowBackgroundColor color) {
    apply_class_background_brush(host_hwnd, color);
    auto widget_result = host.widget();
    if (widget_result.ok() && widget_result.value()) {
        apply_class_background_brush(static_cast<HWND>(widget_result.value()), color);
    }
    apply_webview2_default_background(host, color);
    // Win10 顶边线是叠在标题栏底色上算出来的,主题切换后要重画。
    g_top_border_background = color;
    invalidate_top_border(host_hwnd);
}

// ── Windows 系统文件拖放 + 外部新窗口接管 ─────────────────────────────
// 首选入口是页面把一次 drop 的完整 DOM File 批次作为 WebMessage additional
// objects 发回来。ICoreWebView2File::get_Path 给 native-confirmed absolute path,
// 不信任页面自己拼的 path 字符串,也不会把多选拆成若干不完整的导航事件。
//
// 旧 Runtime / bridge 不可用时仍保留 file:// NavigationStarting / NewWindowRequested
// 拦截:至少防整页跳转 / 打开文件,并为历史单文件拖放提供兼容兜底。新窗口中的
// http(s) 继续交给系统默认浏览器,普通同页导航不受影响。
constexpr wchar_t kWindowsFilesystemDropMessage[] =
    L"acecode:native-filesystem-drop:v1";

bool win_is_file_uri(const std::wstring& uri) {
    if (uri.size() < 5) return false;
    auto lower = [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); };
    return lower(uri[0]) == L'f' && lower(uri[1]) == L'i' && lower(uri[2]) == L'l' &&
           lower(uri[3]) == L'e' && uri[4] == L':';
}

void dispatch_file_uri(const std::wstring& uri) {
    if (g_file_drop_handler) {
        g_file_drop_handler({acecode::wide_to_utf8(uri)}, WebHost::FileDropContext{});
    }
}

std::vector<std::string> win_web_message_file_paths(
    ICoreWebView2WebMessageReceivedEventArgs* args) {
    if (!args) return {};

    LPWSTR raw_message = nullptr;
    const HRESULT message_hr = args->TryGetWebMessageAsString(&raw_message);
    const std::wstring message = raw_message ? raw_message : L"";
    ::CoTaskMemFree(raw_message);
    if (FAILED(message_hr) || message != kWindowsFilesystemDropMessage) return {};

    Microsoft::WRL::ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
    if (
        FAILED(args->QueryInterface(IID_PPV_ARGS(args2.GetAddressOf()))) || !args2) {
        return {};
    }

    Microsoft::WRL::ComPtr<ICoreWebView2ObjectCollectionView> objects;
    if (FAILED(args2->get_AdditionalObjects(objects.GetAddressOf())) || !objects) {
        return {};
    }

    UINT32 count = 0;
    if (FAILED(objects->get_Count(&count)) || count == 0) return {};

    std::vector<std::string> paths;
    paths.reserve(count);
    for (UINT32 index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IUnknown> object;
        if (FAILED(objects->GetValueAtIndex(index, object.GetAddressOf())) || !object) {
            continue;
        }

        Microsoft::WRL::ComPtr<ICoreWebView2File> file;
        if (FAILED(object.As(&file)) || !file) continue;

        LPWSTR raw_path = nullptr;
        const HRESULT path_hr = file->get_Path(&raw_path);
        const std::string path = raw_path ? acecode::wide_to_utf8(raw_path) : "";
        ::CoTaskMemFree(raw_path);
        if (FAILED(path_hr) || path.empty()) continue;
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(path);
        }
    }
    return paths;
}

void install_win_webview_navigation_handlers(webview::webview& host) {
    auto controller_result = host.browser_controller();
    if (!controller_result.ok()) return;
    auto* controller = static_cast<ICoreWebView2Controller*>(controller_result.value());
    if (!controller) return;
    ICoreWebView2* core = nullptr;
    if (FAILED(controller->get_CoreWebView2(&core)) || !core) return;

    using Microsoft::WRL::Callback;
    EventRegistrationToken drop_message_token{};
    core->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*,
               ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                auto paths = win_web_message_file_paths(args);
                if (!paths.empty() && g_file_drop_handler) {
                    g_file_drop_handler(std::move(paths), WebHost::FileDropContext{});
                }
                return S_OK;
            })
            .Get(),
        &drop_message_token);

    EventRegistrationToken nav_token{};
    core->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                if (!args) return S_OK;
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    std::wstring u(uri);
                    ::CoTaskMemFree(uri);
                    if (win_is_file_uri(u)) {
                        args->put_Cancel(TRUE);
                        dispatch_file_uri(u);
                    }
                }
                return S_OK;
            })
            .Get(),
        &nav_token);

    EventRegistrationToken win_token{};
    core->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                if (!args) return S_OK;
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    std::wstring u(uri);
                    ::CoTaskMemFree(uri);
                    if (win_is_file_uri(u)) {
                        args->put_Handled(TRUE);
                        dispatch_file_uri(u);
                        return S_OK;
                    }

                    const std::string external_url = acecode::wide_to_utf8(u);
                    if (is_safe_external_url(external_url)) {
                        // Fail closed: once this is an external new-page request,
                        // never fall back to an embedded WebView popup.
                        args->put_Handled(TRUE);
                        auto result = open_external_url(external_url);
                        if (!result.ok) {
                            LOG_WARN("[desktop] failed to open new-window URL in the "
                                     "system browser: " + result.error);
                        }
                    }
                }
                return S_OK;
            })
            .Get(),
        &win_token);

    core->Release();
}

int win32_hit_test_value(FramelessHitTestArea area) {
    switch (area) {
        // Keep WebView top-bar controls clickable. Dragging is started by the
        // React top bar through aceDesktop_startWindowDrag, so the parent HWND
        // should expose the caption area as client unless it is a resize edge.
        case FramelessHitTestArea::Caption: return HTCLIENT;
        case FramelessHitTestArea::Left: return HTLEFT;
        case FramelessHitTestArea::Right: return HTRIGHT;
        case FramelessHitTestArea::Top: return HTTOP;
        case FramelessHitTestArea::TopLeft: return HTTOPLEFT;
        case FramelessHitTestArea::TopRight: return HTTOPRIGHT;
        case FramelessHitTestArea::Bottom: return HTBOTTOM;
        case FramelessHitTestArea::BottomLeft: return HTBOTTOMLEFT;
        case FramelessHitTestArea::BottomRight: return HTBOTTOMRIGHT;
        case FramelessHitTestArea::Client:
        default: return HTCLIENT;
    }
}

LRESULT frameless_hit_test(HWND hwnd, LPARAM lparam) {
    RECT window{};
    if (!::GetWindowRect(hwnd, &window)) return HTCLIENT;

    const int screen_x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
    const int screen_y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
    const UINT dpi = ::GetDpiForWindow(hwnd);
    FramelessHitTestInput input;
    input.x = screen_x - window.left;
    input.y = screen_y - window.top;
    input.width = window.right - window.left;
    input.height = window.bottom - window.top;
    input.frame_x = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi);
    input.frame_y = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi);
    input.padding = ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    input.drag_height = dpi_scale(kFramelessDragHeightDip, dpi);
    input.maximized = ::IsZoomed(hwnd) != FALSE;
    return win32_hit_test_value(classify_frameless_hit_test(input));
}

LRESULT frameless_nc_calc(HWND hwnd, WPARAM wparam, LPARAM lparam) {
    if (!wparam || !lparam) return ::DefWindowProcW(hwnd, WM_NCCALCSIZE, wparam, lparam);

    LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_THICKFRAME) == 0) {
        return 0;
    }

    const UINT dpi = ::GetDpiForWindow(hwnd);
    const int frame_x = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi);
    const int frame_y = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi);
    const int padding = ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
    RECT& client = params->rgrc[0];
    client.left += frame_x + padding;
    client.right -= frame_x + padding;
    client.bottom -= frame_y + padding;
    if (::IsZoomed(hwnd)) {
        client.top += padding;
    }
    return 0;
}

LRESULT CALLBACK host_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_ACTIVATE: {
            const LRESULT result = call_host_default_proc(hwnd, msg, wparam, lparam);
            if (LOWORD(wparam) != WA_INACTIVE && !::IsIconic(hwnd) &&
                g_window_focus_handler) {
                g_window_focus_handler();
            }
            return result;
        }
        case WM_GETMINMAXINFO:
            apply_minimum_track_size(hwnd, reinterpret_cast<MINMAXINFO*>(lparam));
            return 0;
        case WM_NCCALCSIZE:
            return frameless_nc_calc(hwnd, wparam, lparam);
        case WM_NCHITTEST:
            return frameless_hit_test(hwnd, lparam);
        case WM_NCACTIVATE:
            g_top_border_active = wparam != FALSE;
            invalidate_top_border(hwnd);
            return call_host_default_proc(hwnd, msg, wparam, lparam);
        case WM_PAINT:
            if (paint_top_border(hwnd)) return 0;
            break;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED: {
            // 用户改「在标题栏和窗口边框上显示主题色」或主题色时,顶边线跟着重画。
            const LRESULT result = call_host_default_proc(hwnd, msg, wparam, lparam);
            invalidate_top_border(hwnd);
            return result;
        }
        case WM_SIZE: {
            // 降级路径(webview 库自建窗口)的原窗口过程收到 WM_SIZE 会把 WebView
            // 拉满整个客户区,所以先让它跑完,再按顶边线让位重新摆放。
            const LRESULT result = call_host_default_proc(hwnd, msg, wparam, lparam);
            resize_webview_widget(hwnd);
            notify_window_visibility(wparam != SIZE_MINIMIZED);
            if (g_window_state_handler) {
                const bool maximized = ::IsZoomed(hwnd) != FALSE;
                if (maximized != g_last_known_maximized) {
                    g_last_known_maximized = maximized;
                    g_window_state_handler(maximized);
                }
            }
            return result;
        }
        case WM_SHOWWINDOW: {
            const LRESULT result =
                call_host_default_proc(hwnd, msg, wparam, lparam);
            notify_window_visibility(wparam != FALSE && !::IsIconic(hwnd));
            return result;
        }
        case WM_DPICHANGED:
            if (previous_host_window_proc(hwnd)) {
                ::CallWindowProcW(previous_host_window_proc(hwnd), hwnd, msg, wparam, lparam);
            }
            if (lparam) {
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                ::SetWindowPos(hwnd,
                               nullptr,
                               suggested->left,
                               suggested->top,
                               suggested->right - suggested->left,
                               suggested->bottom - suggested->top,
                               SWP_NOZORDER | SWP_NOACTIVATE);
            }
            resize_webview_widget(hwnd);
            return 0;
        case WM_CLOSE:
            // close handler 返回 true 表示已消化(隐藏到托盘),不要 DestroyWindow。
            // 派发逻辑提取到 web_host_close_policy.hpp 的纯函数,unit test 共用。
            if (dispatch_wm_close(g_close_handler) == CloseDispatch::ConsumedByHandler) {
                return 0;
            }
            if (previous_host_window_proc(hwnd)) {
                return ::CallWindowProcW(previous_host_window_proc(hwnd), hwnd, msg, wparam, lparam);
            }
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (previous_host_window_proc(hwnd)) {
                LRESULT result = ::CallWindowProcW(previous_host_window_proc(hwnd), hwnd, msg, wparam, lparam);
                ::PostQuitMessage(0);
                return result;
            }
            ::PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY: {
            LRESULT result = call_host_default_proc(hwnd, msg, wparam, lparam);
            ::RemovePropW(hwnd, kHostWindowPreviousProcProperty);
            return result;
        }
        default:
            if (msg == kRequestQuitMsg) {
                // 来自 WebHost::request_quit 的真正退出信号 — 绕过 close handler。
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (UINT focus_msg = focus_existing_instance_msg();
                focus_msg != 0 && msg == focus_msg) {
                // 第二个 acecode-desktop 进程检测到单例锁被占,通过 PostMessageW
                // 让我们把窗口拉前。等价于左键单击托盘 + close-to-tray 还原。
                if (::IsIconic(hwnd)) {
                    ::ShowWindow(hwnd, SW_RESTORE);
                } else {
                    ::ShowWindow(hwnd, SW_SHOW);
                }
                ::SetForegroundWindow(hwnd);
                if (g_existing_instance_focus_handler) {
                    g_existing_instance_focus_handler();
                }
                return 0;
            }
            break;
    }
    return call_host_default_proc(hwnd, msg, wparam, lparam);
}

void install_host_window_proc(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd) || previous_host_window_proc(hwnd)) return;
    ::SetLastError(ERROR_SUCCESS);
    LONG_PTR previous = ::SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(host_window_proc));
    if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
        LOG_WARN("[desktop] failed to subclass webview-owned host window, last_error=" +
                 std::to_string(::GetLastError()));
        return;
    }
    ::SetPropW(hwnd,
               kHostWindowPreviousProcProperty,
               reinterpret_cast<HANDLE>(previous));
    // 降级路径的窗口在接管前已被 webview 库显示并激活,错过了那次 WM_NCACTIVATE。
    g_top_border_active = ::GetForegroundWindow() == hwnd;
    refresh_non_client_frame(hwnd);
}

bool register_host_window_class(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpszClassName = kHostWindowClassName;
    wc.lpfnWndProc = host_window_proc;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hIcon = host_window_icons().large_icon;
    wc.hIconSm = host_window_icons().small_icon;
    // 快速 resize 时新暴露区域由类背景刷打底;不设(NULL)= 不擦除 = 黑闪。
    // 默认取前端浅色 body 底色,主题切换后由 apply_class_background_brush 换刷。
    wc.hbrBackground = ::CreateSolidBrush(background_colorref(kDefaultWindowBackground));
    if (::RegisterClassExW(&wc)) return true;
    const DWORD register_error = ::GetLastError();
    if (wc.hbrBackground) {
        ::DeleteObject(wc.hbrBackground);
    }
    return register_error == ERROR_CLASS_ALREADY_EXISTS;
}

HWND create_offscreen_host_window(RECT& target_monitor) {
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (!register_host_window_class(instance)) return nullptr;

    HMONITOR startup_monitor = active_monitor();
    target_monitor = monitor_work_rect(startup_monitor);
    const auto initial_size = fit_desktop_window_to_safe_work_area(
        {kDefaultDesktopWindowWidth, kDefaultDesktopWindowHeight},
        {target_monitor.right - target_monitor.left,
         target_monitor.bottom - target_monitor.top},
        static_cast<int>(monitor_dpi(startup_monitor)));
    const int x = target_monitor.right + 10000;
    const int y = target_monitor.bottom + 10000;
    HWND hwnd = ::CreateWindowExW(
        WS_EX_APPWINDOW,
        kHostWindowClassName,
        L"ACECode",
        WS_OVERLAPPEDWINDOW,
        x,
        y,
        initial_size.width,
        initial_size.height,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (hwnd) {
        if (startup_monitor) {
            ::SetPropW(hwnd, kHostWindowStartupMonitorProperty, startup_monitor);
        }
        refresh_non_client_frame(hwnd);
        // WebView2 initialization for an externally-owned parent HWND is more
        // reliable when the parent is already visible. It is offscreen here,
        // so this does not expose a blank window to the user.
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        ::UpdateWindow(hwnd);
    }
    return hwnd;
}

HMONITOR startup_monitor_for_window(HWND hwnd, const RECT& remembered_work_area) {
    if (remembered_work_area.right > remembered_work_area.left &&
        remembered_work_area.bottom > remembered_work_area.top) {
        POINT center{
            remembered_work_area.left +
                (remembered_work_area.right - remembered_work_area.left) / 2,
            remembered_work_area.top +
                (remembered_work_area.bottom - remembered_work_area.top) / 2,
        };
        if (HMONITOR monitor =
                ::MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST)) {
            return monitor;
        }
    }
    if (hwnd) {
        if (HMONITOR monitor =
                ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)) {
            return monitor;
        }
    }
    return active_monitor();
}

void fit_native_window_to_work_area(HWND hwnd,
                                    const RECT& work_area,
                                    UINT dpi,
                                    bool center) {
    RECT current{};
    if (!hwnd || !::GetWindowRect(hwnd, &current)) return;

    const WindowSize actual_size{
        normalized_window_extent(current.left, current.right),
        normalized_window_extent(current.top, current.bottom),
    };
    if (!center) {
        const WindowSize fitted = fit_desktop_window_to_safe_work_area(
            actual_size,
            {normalized_window_extent(work_area.left, work_area.right),
             normalized_window_extent(work_area.top, work_area.bottom)},
            static_cast<int>(dpi));
        ::SetWindowPos(hwnd,
                       nullptr,
                       0,
                       0,
                       fitted.width,
                       fitted.height,
                       SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        return;
    }

    const WindowRect fitted =
        fit_centered_desktop_window_rect_to_safe_work_area(
            actual_size,
            {work_area.left, work_area.top, work_area.right, work_area.bottom},
            static_cast<int>(dpi));
    ::SetWindowPos(hwnd,
                   nullptr,
                   fitted.left,
                   fitted.top,
                   fitted.right - fitted.left,
                   fitted.bottom - fitted.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace

struct ComApartment {
    explicit ComApartment(bool enable) {
        if (!enable) return;
        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        should_uninitialize = (hr == S_OK || hr == S_FALSE);
    }

    ~ComApartment() {
        if (should_uninitialize) {
            ::CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    bool should_uninitialize = false;
};
#endif

namespace {

#if !defined(_WIN32) && !defined(__APPLE__)
#ifdef ACECODE_DEEPIN
class LinuxWebviewScaleController {
public:
    explicit LinuxWebviewScaleController(webview::webview& host) {
        const char* current_desktop = std::getenv("XDG_CURRENT_DESKTOP");
        const char* session_desktop = std::getenv("XDG_SESSION_DESKTOP");
        if (!is_deepin_desktop(current_desktop ? current_desktop : "") &&
            !is_deepin_desktop(session_desktop ? session_desktop : "")) {
            return;
        }
        const auto widget = host.widget();
        if (!widget.ok() || !widget.value()) return;
        webview_ = WEBKIT_WEB_VIEW(widget.value());
        screen_ = gtk_widget_get_screen(GTK_WIDGET(webview_));
        gtk_settings_ = gtk_widget_get_settings(GTK_WIDGET(webview_));
        if (!gtk_settings_ || !screen_) return;
#ifdef GDK_WINDOWING_X11
        GdkDisplay* display = gdk_screen_get_display(screen_);
        if (!GDK_IS_X11_DISPLAY(display)) return;
        xsettings_atom_ = gdk_x11_get_xatom_by_name_for_display(
            display, "_XSETTINGS_SETTINGS");
        manager_atom_ = gdk_x11_get_xatom_by_name_for_display(display, "MANAGER");
        gdk_window_add_filter(nullptr, native_settings_changed, this);
        filter_installed_ = true;
#else
        return;
#endif

        dpi_signal_ = g_signal_connect(
            gtk_settings_, "notify::gtk-xft-dpi",
            G_CALLBACK(+[](GObject*, GParamSpec*, gpointer context) {
                static_cast<LinuxWebviewScaleController*>(context)->refresh();
            }), this);
        widget_scale_signal_ = g_signal_connect(
            webview_, "notify::scale-factor",
            G_CALLBACK(+[](GObject*, GParamSpec*, gpointer context) {
                static_cast<LinuxWebviewScaleController*>(context)->refresh();
            }), this);
        refresh();
    }

    ~LinuxWebviewScaleController() {
#ifdef GDK_WINDOWING_X11
        if (filter_installed_) {
            gdk_window_remove_filter(nullptr, native_settings_changed, this);
        }
#endif
        if (refresh_source_) g_source_remove(refresh_source_);
        if (dpi_signal_) g_signal_handler_disconnect(gtk_settings_, dpi_signal_);
        if (widget_scale_signal_ &&
            g_signal_handler_is_connected(webview_, widget_scale_signal_)) {
            // A native delete-event can destroy the GTK widget before the
            // host releases its retained WebView reference.
            g_signal_handler_disconnect(webview_, widget_scale_signal_);
        }
        if (active_) {
            gtk_settings_reset_property(gtk_settings_, "gtk-xft-dpi");
        }
        set_linux_tray_font_scale(1.0);
    }

    LinuxWebviewScaleController(const LinuxWebviewScaleController&) = delete;
    LinuxWebviewScaleController& operator=(const LinuxWebviewScaleController&) = delete;

private:
#ifdef GDK_WINDOWING_X11
    static GdkFilterReturn native_settings_changed(
        GdkXEvent* native_event, GdkEvent*, gpointer context) {
        auto* self = static_cast<LinuxWebviewScaleController*>(context);
        const auto* event = static_cast<const XEvent*>(native_event);
        if ((event->type == PropertyNotify &&
             event->xproperty.atom == self->xsettings_atom_) ||
            (event->type == ClientMessage &&
             event->xclient.message_type == self->manager_atom_)) {
            self->schedule_refresh();
        }
        return GDK_FILTER_CONTINUE;
    }
#endif

    void schedule_refresh() {
        if (refresh_source_) return;
        // Let GDK consume the native event and update its XSettings cache first.
        refresh_source_ = g_idle_add(+[](gpointer context) -> gboolean {
            auto* self = static_cast<LinuxWebviewScaleController*>(context);
            self->refresh_source_ = 0;
            self->refresh();
            return G_SOURCE_REMOVE;
        }, this);
    }

    void refresh() {
        if (updating_ || !gtk_settings_ || !screen_ || !webview_) return;
        // GtkSettings is overridden while active. Read the native value below
        // that override; Deepin's scale-factor preference can be stale as well.
        GValue dpi_value = G_VALUE_INIT;
        g_value_init(&dpi_value, G_TYPE_INT);
        const int native_font_dpi = gdk_screen_get_setting(
            screen_, "gtk-xft-dpi", &dpi_value) ? g_value_get_int(&dpi_value) : -1;
        g_value_unset(&dpi_value);
        const int window_scale = gtk_widget_get_scale_factor(GTK_WIDGET(webview_));
        const auto plan = plan_linux_webview_scale(native_font_dpi, window_scale);
        if (native_font_dpi != last_font_dpi_ || window_scale != last_window_scale_) {
            LOG_INFO("[desktop] UOS WebView native font DPI=" +
                     std::to_string(native_font_dpi / 1024.0) +
                     " GTK window scale=" + std::to_string(window_scale) +
                     " page zoom=" + std::to_string(plan.page_zoom));
            last_font_dpi_ = native_font_dpi;
            last_window_scale_ = window_scale;
        }

        updating_ = true;
        if (!plan.apply) {
            if (active_) {
                webkit_web_view_set_zoom_level(webview_, 1.0);
                active_ = false;
                set_linux_tray_font_scale(1.0);
                gtk_settings_reset_property(gtk_settings_, "gtk-xft-dpi");
            }
            updating_ = false;
            return;
        }

        int gtk_font_dpi = -1;
        g_object_get(gtk_settings_, "gtk-xft-dpi", &gtk_font_dpi, nullptr);
        if (gtk_font_dpi != plan.font_dpi) {
            g_object_set(gtk_settings_, "gtk-xft-dpi", plan.font_dpi, nullptr);
        }
        if (!active_ || std::abs(webkit_web_view_get_zoom_level(webview_) -
                                  plan.page_zoom) > 0.001) {
            webkit_web_view_set_zoom_level(webview_, plan.page_zoom);
        }
        active_ = true;
        set_linux_tray_font_scale(native_font_dpi / (96.0 * 1024.0));
        updating_ = false;
    }

    WebKitWebView* webview_ = nullptr;
    GtkSettings* gtk_settings_ = nullptr;
    GdkScreen* screen_ = nullptr;
    guint refresh_source_ = 0;
    gulong dpi_signal_ = 0;
    gulong widget_scale_signal_ = 0;
    int last_font_dpi_ = -1;
    int last_window_scale_ = -1;
#ifdef GDK_WINDOWING_X11
    Atom xsettings_atom_ = 0;
    Atom manager_atom_ = 0;
    bool filter_installed_ = false;
#endif
    bool active_ = false;
    bool updating_ = false;
};
#endif

struct GtkWindowApi {
    using GtkWidgetShow = void (*)(void*);
    using GtkWidgetHide = void (*)(void*);
    using GtkWindowSetDecorated = void (*)(void*, int);
    using GtkWindowPresent = void (*)(void*);
    using GtkWindowClose = void (*)(void*);
    using GtkWindowBeginMoveDrag = void (*)(void*, int, int, int, unsigned int);
    using GtkWindowBeginResizeDrag = void (*)(void*, int, int, int, int, unsigned int);
    using GtkWindowIconify = void (*)(void*);
    using GtkWindowMaximize = void (*)(void*);
    using GtkWindowUnmaximize = void (*)(void*);
    using GtkWindowGetWindow = void* (*)(void*);
    using GdkWindowGetState = int (*)(void*);
    using GSignalConnectData = unsigned long (*)(void*, const char*, void*, void*, void*, int);

    void* gtk = nullptr;
    void* gdk = nullptr;
    void* gobject = nullptr;
    GtkWidgetShow widget_show = nullptr;
    GtkWidgetHide widget_hide = nullptr;
    GtkWindowSetDecorated window_set_decorated = nullptr;
    GtkWindowPresent window_present = nullptr;
    GtkWindowClose window_close = nullptr;
    GtkWindowBeginMoveDrag window_begin_move_drag = nullptr;
    GtkWindowBeginResizeDrag window_begin_resize_drag = nullptr;
    GtkWindowIconify window_iconify = nullptr;
    GtkWindowMaximize window_maximize = nullptr;
    GtkWindowUnmaximize window_unmaximize = nullptr;
    GtkWindowGetWindow window_get_window = nullptr;
    GdkWindowGetState gdk_window_get_state = nullptr;
    GSignalConnectData signal_connect_data = nullptr;

    bool load() {
        if (gtk && gdk && gobject) return true;
        gtk = ::dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_LOCAL);
        gdk = ::dlopen("libgdk-3.so.0", RTLD_LAZY | RTLD_LOCAL);
        gobject = ::dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!gtk || !gdk || !gobject) {
            LOG_WARN("[desktop] GTK3/GDK/GObject runtime not available for window controls");
            return false;
        }
        auto sym = [](void* lib, const char* name) -> void* {
            return ::dlsym(lib, name);
        };
        widget_show = reinterpret_cast<GtkWidgetShow>(sym(gtk, "gtk_widget_show"));
        widget_hide = reinterpret_cast<GtkWidgetHide>(sym(gtk, "gtk_widget_hide"));
        window_set_decorated = reinterpret_cast<GtkWindowSetDecorated>(
            sym(gtk, "gtk_window_set_decorated"));
        window_present = reinterpret_cast<GtkWindowPresent>(sym(gtk, "gtk_window_present"));
        window_close = reinterpret_cast<GtkWindowClose>(sym(gtk, "gtk_window_close"));
        window_begin_move_drag = reinterpret_cast<GtkWindowBeginMoveDrag>(
            sym(gtk, "gtk_window_begin_move_drag"));
        window_begin_resize_drag = reinterpret_cast<GtkWindowBeginResizeDrag>(
            sym(gtk, "gtk_window_begin_resize_drag"));
        window_iconify = reinterpret_cast<GtkWindowIconify>(sym(gtk, "gtk_window_iconify"));
        window_maximize = reinterpret_cast<GtkWindowMaximize>(sym(gtk, "gtk_window_maximize"));
        window_unmaximize = reinterpret_cast<GtkWindowUnmaximize>(
            sym(gtk, "gtk_window_unmaximize"));
        window_get_window = reinterpret_cast<GtkWindowGetWindow>(
            sym(gtk, "gtk_widget_get_window"));
        gdk_window_get_state = reinterpret_cast<GdkWindowGetState>(
            sym(gdk, "gdk_window_get_state"));
        signal_connect_data = reinterpret_cast<GSignalConnectData>(
            sym(gobject, "g_signal_connect_data"));
        return widget_show && widget_hide && window_set_decorated && window_present &&
               window_close && window_begin_move_drag && window_begin_resize_drag &&
               window_iconify && window_maximize && window_unmaximize && window_get_window &&
               gdk_window_get_state && signal_connect_data;
    }
};

GtkWindowApi& gtk_window_api() {
    static GtkWindowApi api;
    return api;
}

bool g_linux_force_close = false;
std::function<void(bool)> g_linux_window_state_handler;
bool g_linux_last_known_maximized = false;

constexpr int kGdkWindowStateMaximized = 1 << 2;

struct GdkEventWindowStateCompat {
    int type;
    void* window;
    signed char send_event;
    int changed_mask;
    int new_window_state;
};

extern "C" int linux_window_delete_event(void*, void*, void*) {
    if (g_linux_force_close) return 0;
    return dispatch_wm_close(g_close_handler) == CloseDispatch::ConsumedByHandler ? 1 : 0;
}

extern "C" int linux_window_state_event(void*, GdkEventWindowStateCompat* event, void*) {
    const bool maximized = event &&
        ((event->new_window_state & kGdkWindowStateMaximized) != 0);
    if (maximized != g_linux_last_known_maximized) {
        g_linux_last_known_maximized = maximized;
        if (g_linux_window_state_handler) {
            g_linux_window_state_handler(maximized);
        }
    }
    return 0;
}

void install_linux_close_handler(webview::webview& w) {
    auto window = w.window();
    if (!window.ok() || !window.value()) return;
    auto& api = gtk_window_api();
    if (!api.load()) return;
    api.signal_connect_data(window.value(),
                            "delete-event",
                            reinterpret_cast<void*>(linux_window_delete_event),
                            nullptr,
                            nullptr,
                            0);
}

void install_linux_window_state_handler(webview::webview& w) {
    auto window = w.window();
    if (!window.ok() || !window.value()) return;
    auto& api = gtk_window_api();
    if (!api.load()) return;
    api.signal_connect_data(window.value(),
                            "window-state-event",
                            reinterpret_cast<void*>(linux_window_state_event),
                            nullptr,
                            nullptr,
                            0);
}

void configure_linux_window_chrome(webview::webview& w) {
    auto window = w.window();
    if (!window.ok() || !window.value()) return;
    auto& api = gtk_window_api();
    if (!api.load()) return;
    api.window_set_decorated(window.value(), 0);
}

bool linux_window_is_maximized(void* window) {
    if (!window) return false;
    auto& api = gtk_window_api();
    if (!api.load()) return false;
    void* gdk_window = api.window_get_window(window);
    if (!gdk_window) return g_linux_last_known_maximized;
    return (api.gdk_window_get_state(gdk_window) & kGdkWindowStateMaximized) != 0;
}

std::optional<int> gtk_resize_edge_for_direction(const std::string& direction) {
    if (direction == "top-left") return 0;      // GDK_WINDOW_EDGE_NORTH_WEST
    if (direction == "top") return 1;           // GDK_WINDOW_EDGE_NORTH
    if (direction == "top-right") return 2;     // GDK_WINDOW_EDGE_NORTH_EAST
    if (direction == "left") return 3;          // GDK_WINDOW_EDGE_WEST
    if (direction == "right") return 4;         // GDK_WINDOW_EDGE_EAST
    if (direction == "bottom-left") return 5;   // GDK_WINDOW_EDGE_SOUTH_WEST
    if (direction == "bottom") return 6;        // GDK_WINDOW_EDGE_SOUTH
    if (direction == "bottom-right") return 7;  // GDK_WINDOW_EDGE_SOUTH_EAST
    return std::nullopt;
}
#endif

std::pair<int, int> adjusted_window_size(int width, int height) {
#ifdef __APPLE__
    CGRect bounds = CGDisplayBounds(CGMainDisplayID());
    const double display_width = bounds.size.width;
    const double display_height = bounds.size.height;
    if (display_width > 0 && display_height > 0) {
        auto clamp_dimension = [](int value, double display, double margin, int preferred_min) {
            const int max_value = static_cast<int>(std::max(1.0, display - margin));
            const int min_value = std::min(preferred_min, max_value);
            return std::max(min_value, std::min(value, max_value));
        };
        width = clamp_dimension(width, display_width, 80.0, 900);
        height = clamp_dimension(height, display_height, 120.0, 640);
    }
#endif
    return {width, height};
}

std::string join_version(unsigned int major, unsigned int minor, unsigned int micro) {
    std::ostringstream oss;
    oss << major << "." << minor << "." << micro;
    return oss.str();
}

std::string webview_wrapper_version() {
    const auto* version = webview_version();
    if (!version || version->version_number[0] == '\0') return {};
    return version->version_number;
}

WebHost::WebCoreInfo make_web_core_info_base() {
    WebHost::WebCoreInfo info;
    info.wrapper_name = "webview";
    info.wrapper_version = webview_wrapper_version();
    return info;
}

#ifdef _WIN32
std::wstring getenv_wide(const wchar_t* name) {
    DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(static_cast<std::size_t>(needed), L'\0');
    DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) return {};
    value.resize(static_cast<std::size_t>(written));
    return value;
}

std::wstring trim_trailing_path_separators(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

std::wstring last_path_component(std::wstring path) {
    path = trim_trailing_path_separators(std::move(path));
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

std::wstring query_registry_string(HKEY root,
                                   const wchar_t* sub_key,
                                   const wchar_t* value_name,
                                   REGSAM view_flags) {
    HKEY key = nullptr;
    LONG opened = ::RegOpenKeyExW(root, sub_key, 0, KEY_READ | view_flags, &key);
    if (opened != ERROR_SUCCESS || !key) return {};

    DWORD type = 0;
    DWORD bytes = 0;
    LONG sized = ::RegQueryValueExW(
        key, value_name, nullptr, &type, nullptr, &bytes);
    if (sized != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) ||
        bytes < sizeof(wchar_t)) {
        ::RegCloseKey(key);
        return {};
    }

    std::wstring value(static_cast<std::size_t>(bytes / sizeof(wchar_t)), L'\0');
    LONG read = ::RegQueryValueExW(
        key,
        value_name,
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(value.data()),
        &bytes);
    ::RegCloseKey(key);
    if (read != ERROR_SUCCESS) return {};

    value.resize(static_cast<std::size_t>(bytes / sizeof(wchar_t)));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

std::wstring installed_webview2_runtime_folder() {
    constexpr const wchar_t* kClientStateKey =
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\"
        L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    constexpr const wchar_t* kEbWebViewValue = L"EBWebView";
    const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    const REGSAM views[] = {KEY_WOW64_32KEY, 0};
    for (HKEY root : roots) {
        for (REGSAM view : views) {
            auto value = query_registry_string(root, kClientStateKey, kEbWebViewValue, view);
            if (!value.empty()) return value;
        }
    }
    return {};
}

WebHost::WebCoreInfo detect_platform_web_core_info() {
    auto info = make_web_core_info_base();
    info.backend = "webview2";
    info.name = "WebView2";

    const std::wstring env_folder =
        getenv_wide(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER");
    if (!env_folder.empty()) {
        info.version = acecode::wide_to_utf8(last_path_component(env_folder));
        info.detail = "Edge/WebView2 folder runtime";
        info.runtime_path = acecode::wide_to_utf8(env_folder);
        return info;
    }

    const std::wstring runtime_folder = installed_webview2_runtime_folder();
    if (!runtime_folder.empty()) {
        info.version = acecode::wide_to_utf8(last_path_component(runtime_folder));
        info.detail = "Evergreen Runtime";
        info.runtime_path = acecode::wide_to_utf8(runtime_folder);
        return info;
    }

    info.detail = "runtime available";
    return info;
}
#elif defined(__APPLE__)
std::string nsstring_to_utf8(NSString* value) {
    if (!value) return {};
    const char* text = [value UTF8String];
    return text ? std::string(text) : std::string();
}

WebHost::WebCoreInfo detect_platform_web_core_info() {
    auto info = make_web_core_info_base();
    info.backend = "wkwebview";
    info.name = "WKWebView";
    info.detail = "WebKit framework";
    NSBundle* webkit = [NSBundle bundleWithIdentifier:@"com.apple.WebKit"];
    if (webkit) {
        id value = [webkit objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
        if ([value isKindOfClass:[NSString class]]) {
            info.version = nsstring_to_utf8((NSString*)value);
        }
    }
    return info;
}
#else
WebHost::WebCoreInfo detect_platform_web_core_info() {
    auto info = make_web_core_info_base();
    info.backend = "webkitgtk";
    info.name = "WebKitGTK";
    info.version = join_version(
        webkit_get_major_version(),
        webkit_get_minor_version(),
        webkit_get_micro_version());
    info.detail = "GTK " + join_version(
        gtk_get_major_version(),
        gtk_get_minor_version(),
        gtk_get_micro_version());
    return info;
}
#endif

} // namespace

struct WebHost::Impl {
    explicit Impl(bool debug, StartupWindowMode startup_mode)
#ifdef _WIN32
        : custom_window(startup_mode == StartupWindowMode::OffscreenUntilReady
                            ? create_offscreen_host_window(target_monitor)
                            : nullptr),
          center_on_first_show(
              startup_mode == StartupWindowMode::OffscreenUntilReady),
          com(custom_window != nullptr) {
        // 三段式构造:
        //   (1) 默认 Loader 路径,优先 offscreen custom_window;失败 → 切
        //       自管 nullptr 父窗口再试一次(沿用现有降级)。
        //   (2) (1) 整段还是抛 → 探测 Edge 浏览器 / EdgeWebView Runtime
        //       自带的 msedgewebview2.exe 目录,通过
        //       WEBVIEW2_BROWSER_EXECUTABLE_FOLDER 环境变量(WebView2Loader.dll
        //       公开的覆盖钩子)指过去再试。
        //   (3) Edge fallback 仍失败或没找到 Edge → 抛 WebHostInitializationError
        //       给 desktop main。main 还有最后一层 Edge --app=<daemon URL>
        //       兜底,不能在 WebHost 构造函数里直接 ExitProcess。

        // WebView2 创建之前把默认打底色钉进环境变量。官方文档:仅靠
        // put_DefaultBackgroundColor 属性,首帧之前仍会闪默认白;环境变量是
        // 唯一覆盖「首次渲染之前」窗口的通道,且只对首次环境创建生效,运行时
        // 主题切换走 apply_webview2_default_background 的属性更新。
        {
            const std::string env_value =
                webview2_default_background_env_value(kDefaultWindowBackground);
            const std::wstring env_value_w(env_value.begin(), env_value.end());
            ::SetEnvironmentVariableW(L"WEBVIEW2_DEFAULT_BACKGROUND_COLOR",
                                      env_value_w.c_str());
        }

        auto make_webview_default_path = [&]() -> std::unique_ptr<webview::webview> {
            try {
                return std::make_unique<webview::webview>(
                    debug,
                    custom_window ? static_cast<void*>(custom_window) : nullptr);
            } catch (const webview::exception& e) {
                if (!custom_window) throw;
                LOG_WARN(std::string("[desktop] offscreen WebView host failed; "
                                     "falling back to default window: ") + e.what());
                if (::IsWindow(custom_window)) {
                    ::DestroyWindow(custom_window);
                }
                custom_window = nullptr;
                return std::make_unique<webview::webview>(debug, nullptr);
            }
        };

        try {
            w = make_webview_default_path();
        } catch (const webview::exception& e1) {
            LOG_WARN(std::string("[desktop] WebView2 default loader path failed: ") +
                     e1.what());
            auto edge_folder = find_edge_browser_folder();
            if (!edge_folder.has_value()) {
                LOG_ERROR("[desktop] no Microsoft Edge/WebView2 executable folder "
                          "found to fall back to embedded WebView2");
                throw WebHostInitializationError(
                    std::string("WebView2 default loader path failed and no "
                                "Edge/WebView2 executable folder was found: ") +
                    e1.what());
            }
            const std::wstring folder_w = edge_folder->wstring();
            LOG_INFO(std::string("[desktop] retrying WebView2 with Edge browser "
                                 "or WebView2 folder: ") + edge_folder->string());
            if (!::SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",
                                           folder_w.c_str())) {
                LOG_ERROR("[desktop] SetEnvironmentVariableW("
                          "WEBVIEW2_BROWSER_EXECUTABLE_FOLDER) failed, last_error=" +
                          std::to_string(::GetLastError()));
            }
            try {
                // custom_window 在 default 路径内已被清掉(如果走过 offscreen),
                // 这里直接喂 nullptr 让 webview 自己造窗口最稳妥。
                w = std::make_unique<webview::webview>(debug, nullptr);
            } catch (const webview::exception& e2) {
                LOG_ERROR(std::string("[desktop] WebView2 Edge/WebView2 folder "
                                      "fallback also failed: ") + e2.what());
                throw WebHostInitializationError(
                    std::string("WebView2 Edge/WebView2 folder fallback failed: ") +
                    e2.what());
            }
        }
        if (custom_window) {
            resize_webview_widget(custom_window);
        } else if (w) {
            HWND owned_window = hwnd();
            install_host_window_proc(owned_window);
        }
        if (w) {
            // Also cover the WebView-owned fallback window, whose class has no icon.
            apply_host_window_icons(hwnd());
            taskbar_badge = std::make_unique<WindowsTaskbarBadge>(hwnd());
            configure_browser_defaults(*w);
            // 三层打底(host 类刷 / widget 类刷 / WebView2 合成器)统一走默认色。
            // offscreen 路径的 host 类注册时已带刷,这里等价换新;降级路径
            // (webview 库自建 `webview` 类窗口)的 NULL 刷靠这里补上。
            apply_window_background(*w, hwnd(), kDefaultWindowBackground);
        }
    }
#else
    {
        (void)startup_mode;
#if defined(__APPLE__)
        w = std::make_unique<webview::webview>(debug, nullptr);
        configure_mac_window_chrome(*w);
        install_mac_edit_menu(*w);
        mac_focus_observer = install_mac_focus_existing_observer(*w);
        mac_enter_fullscreen_observer = install_mac_window_fullscreen_observer(
            *w, NSWindowDidEnterFullScreenNotification);
        mac_exit_fullscreen_observer = install_mac_window_fullscreen_observer(
            *w, NSWindowDidExitFullScreenNotification);
        install_mac_close_handler(*w);
        install_mac_application_reopen_handler(mac_window_from_host(*w));
#else
        w = std::make_unique<webview::webview>(debug, nullptr);
        auto native_window = w->window();
        set_linux_folder_picker(pick_linux_folder);
        linux_center_on_first_show = true;
        if (native_window.ok() && native_window.value() &&
            startup_mode == StartupWindowMode::OffscreenUntilReady) {
            auto* widget = GTK_WIDGET(native_window.value());
            // Keep WebKit mapped so startup animation frames can complete.
            // Hide it visually before DTK synchronizes the GTK event queue.
            gtk_widget_set_opacity(widget, 0.0);
            gtk_window_set_accept_focus(GTK_WINDOW(widget), FALSE);
            gtk_window_set_focus_on_map(GTK_WINDOW(widget), FALSE);
        }
        if (native_window.ok() && native_window.value() &&
            !set_linux_window_icon(native_window.value(), application_icon_path())) {
            LOG_WARN("[desktop] could not load the Linux application window icon");
        }
        configure_linux_window_chrome(*w);
#ifdef ACECODE_DEEPIN
        if (native_window.ok()) {
            deepin_effects = std::make_unique<DeepinWindowEffects>(native_window.value());
        }
        linux_scale = std::make_unique<LinuxWebviewScaleController>(*w);
#endif
        install_linux_close_handler(*w);
        install_linux_window_state_handler(*w);
#endif
    }
#endif

    ~Impl() {
#ifdef _WIN32
        taskbar_badge.reset();
        HWND hwnd = custom_window;
#endif
        // Destroy webview first; for m_owns_window=false it removes only the child widget.
        // The parent HWND remains ours and is destroyed below.
#ifdef __APPLE__
        if (g_mac_quit_webview == w.get()) {
            g_mac_quit_webview = nullptr;
        }
        if (g_mac_reopen_window == mac_window_from_host(*w)) {
            g_mac_reopen_window = nil;
        }
        if (mac_focus_observer) {
            [[NSDistributedNotificationCenter defaultCenter] removeObserver:mac_focus_observer];
            mac_focus_observer = nil;
        }
        if (mac_enter_fullscreen_observer) {
            [[NSNotificationCenter defaultCenter]
                removeObserver:mac_enter_fullscreen_observer];
            mac_enter_fullscreen_observer = nil;
        }
        if (mac_exit_fullscreen_observer) {
            [[NSNotificationCenter defaultCenter]
                removeObserver:mac_exit_fullscreen_observer];
            mac_exit_fullscreen_observer = nil;
        }
#endif
        // The controller owns GTK signals that reference the WebView widget.
#ifdef ACECODE_DEEPIN
        deepin_effects.reset();
        linux_scale.reset();
#endif
        w.reset();
#ifdef _WIN32
        if (hwnd && ::IsWindow(hwnd)) {
            ::DestroyWindow(hwnd);
        }
#endif
    }

#ifdef _WIN32
    RECT target_monitor{};
    HWND custom_window = nullptr;
    bool center_on_first_show = false;
    ComApartment com{false};
    std::unique_ptr<WindowsTaskbarBadge> taskbar_badge;

    HWND hwnd() const {
        if (custom_window) return custom_window;
        if (!w) return nullptr;
        auto r = w->window();
        return r.ok() ? static_cast<HWND>(r.value()) : nullptr;
    }
#endif
    std::unique_ptr<webview::webview> w;
#if !defined(_WIN32) && !defined(__APPLE__)
    bool linux_center_on_first_show = false;
#endif
#ifdef ACECODE_DEEPIN
    std::unique_ptr<LinuxWebviewScaleController> linux_scale;
    std::unique_ptr<DeepinWindowEffects> deepin_effects;
#endif
#ifdef __APPLE__
    id mac_focus_observer = nil;
    id mac_enter_fullscreen_observer = nil;
    id mac_exit_fullscreen_observer = nil;
#endif
};

WebHost::WebHost(bool debug, StartupWindowMode startup_mode)
    : impl_(new Impl(debug, startup_mode)) {}
WebHost::~WebHost() { delete impl_; }

void WebHost::set_title(const std::string& title) {
    impl_->w->set_title(title);
}
void WebHost::set_size(int width, int height) {
    auto adjusted = adjusted_window_size(width, height);
#ifdef __APPLE__
    NSWindow* window = mac_window_from_host(*impl_->w);
    if (!window) return;
    [window setFrame:NSMakeRect(0, 0, adjusted.first, adjusted.second)
             display:YES
             animate:NO];
    [window center];
    [window makeKeyAndOrderFront:nil];
    configure_mac_window_chrome(*impl_->w);
#else
#ifdef _WIN32
    RECT work_area = impl_->target_monitor;
    HMONITOR target_monitor =
        startup_monitor_for_window(impl_->hwnd(), work_area);
    if (work_area.right <= work_area.left || work_area.bottom <= work_area.top) {
        work_area = monitor_work_rect(target_monitor);
    }
    const auto clamped = fit_desktop_window_to_safe_work_area(
        {adjusted.first, adjusted.second},
        {work_area.right - work_area.left, work_area.bottom - work_area.top},
        static_cast<int>(monitor_dpi(target_monitor)));
    adjusted = {clamped.width, clamped.height};
#endif
    impl_->w->set_size(adjusted.first, adjusted.second, WEBVIEW_HINT_NONE);
#ifdef _WIN32
    if (HWND hwnd = impl_->hwnd()) {
        // webview::set_size treats its input as 96-DPI logical client size and
        // can therefore enlarge the actual outer HWND after our initial clamp.
        // Fit that post-scaling native size before it can become visible.
        HMONITOR monitor =
            startup_monitor_for_window(hwnd, impl_->target_monitor);
        const RECT live_work_area = monitor_work_rect(monitor);
        fit_native_window_to_work_area(
            hwnd, live_work_area, monitor_dpi(monitor), /*center=*/false);
        resize_webview_widget(hwnd);
    }
#endif
#endif
}
void WebHost::navigate(const std::string& url) {
    impl_->w->navigate(url);
}
void WebHost::set_visible(bool visible) {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd) return;
    if (visible && impl_->center_on_first_show) {
        // Re-read rcWork at the last possible moment so taskbar size/position
        // changes during startup are reflected in the first visible rectangle.
        HMONITOR monitor =
            startup_monitor_for_window(hwnd, impl_->target_monitor);
        const RECT live_work_area = monitor_work_rect(monitor);
        fit_native_window_to_work_area(
            hwnd, live_work_area, monitor_dpi(monitor), /*center=*/true);
        impl_->target_monitor = live_work_area;
        impl_->center_on_first_show = false;
        ::RemovePropW(hwnd, kHostWindowStartupMonitorProperty);
    }
    ::ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
    if (visible) {
        ::UpdateWindow(hwnd);
        ::SetForegroundWindow(hwnd);
    }
#else
#if !defined(__APPLE__)
    auto window = impl_->w->window();
    if (!window.ok() || !window.value()) return;
    auto& api = gtk_window_api();
    if (!api.load()) return;
    if (visible) {
        if (impl_->linux_center_on_first_show) {
            center_linux_window(window.value(), linux_active_work_area());
            impl_->linux_center_on_first_show = false;
        }
        gtk_widget_set_opacity(GTK_WIDGET(window.value()), 1.0);
        gtk_window_set_accept_focus(GTK_WINDOW(window.value()), TRUE);
        gtk_window_set_focus_on_map(GTK_WINDOW(window.value()), TRUE);
        api.widget_show(window.value());
        api.window_present(window.value());
    } else {
        api.widget_hide(window.value());
    }
#else
    auto window_result = impl_->w->window();
    if (!window_result.ok() || !window_result.value()) return;
    NSWindow* window = static_cast<NSWindow*>(window_result.value());
    if (visible) {
        show_mac_window(window);
    } else {
        hide_mac_window_to_tray(window);
    }
#endif
#endif
}
void WebHost::init_script(const std::string& js) {
    impl_->w->init(js);
}
void WebHost::eval(const std::string& js) {
    // webview C++ 的 eval 内部最终走平台 InvokeScript 调用,不一定保证跨线程
    // 安全。dispatch 把 eval 调度到 webview 主循环线程,跨线程 caller 安全。
    auto js_copy = js;
    auto* w_ptr = impl_->w.get();
    impl_->w->dispatch([w_ptr, js_copy] {
        w_ptr->eval(js_copy);
    });
}
void WebHost::dispatch(std::function<void()> task) {
    if (!task) return;
    impl_->w->dispatch(std::move(task));
}
bool WebHost::focus_after_file_drop() {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd) return false;
    const DWORD current_thread = ::GetCurrentThreadId();
    const DWORD foreground_thread =
        ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
    // The drag source owns the last native input. Briefly share its input
    // queue for this explicit drop, then detach before returning to WebView.
    const bool attached = foreground_thread && foreground_thread != current_thread &&
        ::AttachThreadInput(current_thread, foreground_thread, TRUE) != FALSE;
    set_visible(true);
    if (attached) ::AttachThreadInput(current_thread, foreground_thread, FALSE);
    if (::GetForegroundWindow() != hwnd) return false;

    auto controller_result = impl_->w->browser_controller();
    if (!controller_result.ok()) return false;
    auto* controller = static_cast<ICoreWebView2Controller*>(controller_result.value());
    return controller && SUCCEEDED(
        controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC));
#else
    set_visible(true);
    return true;
#endif
}

bool WebHost::open_dev_tools() {
#ifdef _WIN32
    auto controller_result = impl_->w->browser_controller();
    if (!controller_result.ok()) return false;
    auto* controller = static_cast<ICoreWebView2Controller*>(controller_result.value());
    if (!controller) return false;

    ICoreWebView2* webview = nullptr;
    HRESULT hr = controller->get_CoreWebView2(&webview);
    if (FAILED(hr) || !webview) return false;
    const bool ok = open_dev_tools_for_core(webview);
    webview->Release();
    return ok;
#else
    return false;
#endif
}

bool WebHost::set_taskbar_badge(const TaskbarBadge& badge) {
#ifdef _WIN32
    return impl_->taskbar_badge && impl_->taskbar_badge->set(badge);
#else
    (void)badge;
    return false;
#endif
}

bool WebHost::set_background_color(const std::string& color_text) {
#ifdef _WIN32
    auto color = parse_window_background_color(color_text);
    if (!color) return false;
    if (!impl_->w) return false;
    apply_window_background(*impl_->w, impl_->hwnd(), *color);
    return true;
#else
    // v1 只覆盖 Windows(WebView2 的 resize 黑边是本 bridge 存在的动因;
    // WKWebView/WebKitGTK 的 resize 重绘同步得多)。非 Windows 返回 false,
    // 前端 fire-and-forget 静默吞。
    (void)color_text;
    return false;
#endif
}
bool WebHost::start_window_drag(const PointerEvent& event) {
#ifdef _WIN32
    (void)event;
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    ::ReleaseCapture();
    ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    return true;
#else
#if !defined(__APPLE__)
    auto window = impl_->w->window();
    if (!window.ok() || !window.value()) return false;
    auto& api = gtk_window_api();
    if (!api.load()) return false;
    api.window_begin_move_drag(window.value(),
                               event.button <= 0 ? 1 : event.button,
                               event.root_x,
                               event.root_y,
                               event.timestamp);
    return true;
#else
    (void)event;
    return mac_perform_window_drag(mac_window_from_host(*impl_->w));
#endif
#endif
}
bool WebHost::start_window_resize(const std::string& direction,
                                  const PointerEvent& event) {
#ifdef _WIN32
    (void)event;
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    // 最大化窗口走 native resize 会被 Windows 解读为"拖出还原",体验诡异;
    // 直接拒绝,前端 strip 在最大化时也不应渲染。
    if (::IsZoomed(hwnd)) return false;
    auto area = parse_resize_direction(direction);
    if (!area) return false;
    const int ht = win32_hit_test_value(*area);
    // Caption / Client 不是 resize 命中,parse_resize_direction 已经过滤掉,
    // 这里二次保险:任何不是 resize 边/角的值都不发消息。
    switch (ht) {
        case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
        case HTTOPLEFT: case HTTOPRIGHT:
        case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
            break;
        default:
            return false;
    }
    ::ReleaseCapture();
    ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, ht, 0);
    return true;
#else
#if !defined(__APPLE__)
    auto edge = gtk_resize_edge_for_direction(direction);
    if (!edge) return false;
    auto window = impl_->w->window();
    if (!window.ok() || !window.value()) return false;
    if (linux_window_is_maximized(window.value())) return false;
    auto& api = gtk_window_api();
    if (!api.load()) return false;
    api.window_begin_resize_drag(window.value(),
                                 *edge,
                                 event.button <= 0 ? 1 : event.button,
                                 event.root_x,
                                 event.root_y,
                                 event.timestamp);
    return true;
#else
    (void)event;
    auto area = parse_resize_direction(direction);
    if (!area) return false;
    return mac_track_window_resize(mac_window_from_host(*impl_->w), *area);
#endif
#endif
}
bool WebHost::minimize_window() {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    ::ShowWindow(hwnd, SW_MINIMIZE);
    return true;
#else
#if !defined(__APPLE__)
    auto window = impl_->w->window();
    if (!window.ok() || !window.value()) return false;
    auto& api = gtk_window_api();
    if (!api.load()) return false;
    api.window_iconify(window.value());
    return true;
#else
    NSWindow* window = mac_window_from_host(*impl_->w);
    if (!window) return false;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [window makeKeyAndOrderFront:nil];
    [window miniaturize:nil];
    return true;
#endif
#endif
}
bool WebHost::toggle_maximize_window() {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    ::ShowWindow(hwnd, ::IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    return true;
#else
#if !defined(__APPLE__)
    auto window = impl_->w->window();
    if (!window.ok() || !window.value()) return false;
    auto& api = gtk_window_api();
    if (!api.load()) return false;
    const bool maximized = linux_window_is_maximized(window.value());
    if (maximized) {
        api.window_unmaximize(window.value());
    } else {
        api.window_maximize(window.value());
    }
    g_linux_last_known_maximized = !maximized;
    if (g_linux_window_state_handler) {
        g_linux_window_state_handler(!maximized);
    }
    return true;
#else
    NSWindow* window = mac_window_from_host(*impl_->w);
    if (!window) return false;
    [window zoom:nil];
    notify_mac_window_state_if_changed(window);
    return true;
#endif
#endif
}
bool WebHost::is_window_maximized() const {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    return ::IsZoomed(hwnd) != FALSE;
#else
#if !defined(__APPLE__)
    auto window = impl_->w->window();
    if (!window.ok() || !window.value()) return false;
    return linux_window_is_maximized(window.value());
#else
    NSWindow* window = mac_window_from_host(*impl_->w);
    return window && [window isZoomed] == YES;
#endif
#endif
}
bool WebHost::is_window_fullscreen() const {
#ifdef __APPLE__
    return mac_window_is_fullscreen(mac_window_from_host(*impl_->w));
#else
    return false;
#endif
}
WebHost::WebCoreInfo WebHost::web_core_info() const {
    (void)impl_;
    return detect_platform_web_core_info();
}
void WebHost::set_window_state_change_handler(WindowStateHandler handler) {
#ifdef _WIN32
    g_window_state_handler = std::move(handler);
#else
#if !defined(__APPLE__)
    g_linux_window_state_handler = std::move(handler);
#else
    g_mac_window_state_handler = std::move(handler);
#endif
#endif
}
void WebHost::set_window_fullscreen_change_handler(
    WindowFullscreenHandler handler) {
#ifdef __APPLE__
    g_mac_window_fullscreen_handler = std::move(handler);
#else
    (void)handler;
#endif
}
void WebHost::set_window_visibility_handler(WindowVisibilityHandler handler) {
#ifdef _WIN32
    g_window_visibility_handler = std::move(handler);
#else
    (void)handler;
#endif
}
void WebHost::set_window_focus_handler(WindowFocusHandler handler) {
#ifdef _WIN32
    g_window_focus_handler = std::move(handler);
#else
    (void)handler;
#endif
}
void WebHost::set_existing_instance_focus_handler(
    ExistingInstanceFocusHandler handler) {
    g_existing_instance_focus_handler = std::move(handler);
}
bool WebHost::close_window() {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    return ::PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE;
#else
#if !defined(__APPLE__)
    if (dispatch_wm_close(g_close_handler) == CloseDispatch::ConsumedByHandler) {
        return true;
    }
    auto window = impl_->w->window();
    if (!window.ok() || !window.value()) return false;
    auto& api = gtk_window_api();
    if (!api.load()) return false;
    api.window_close(window.value());
    return true;
#else
    if (dispatch_wm_close(g_close_handler) == CloseDispatch::ConsumedByHandler) {
        return true;
    }
    auto window_result = impl_->w->window();
    if (!window_result.ok() || !window_result.value()) return false;
    NSWindow* window = static_cast<NSWindow*>(window_result.value());
    [window close];
    return true;
#endif
#endif
}
void WebHost::set_close_request_handler(std::function<bool()> handler) {
    g_close_handler = std::move(handler);
}
void WebHost::set_file_drop_handler(FileDropHandler handler) {
    g_file_drop_handler = std::move(handler);
#ifdef _WIN32
    install_win_webview_navigation_handlers(*impl_->w);
#elif defined(__APPLE__)
    install_mac_file_drop(*impl_->w);
#endif
    // Linux/WebKitGTK:前端经 text/uri-list 处理,native 不安装拦截。
}
acecode::ClipboardPathsReadResult WebHost::read_clipboard_paths() {
    using Result = acecode::ClipboardPathsReadResult;
#ifdef _WIN32
    return acecode::read_system_clipboard_paths();
#else
    Result result;
    result.status = Result::Status::Empty;
#ifdef __APPLE__
    NSArray<NSURL*>* urls = [[NSPasteboard generalPasteboard]
        readObjectsForClasses:@[[NSURL class]]
        options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL* url in urls) {
        const char* path = [url.path UTF8String];
        if (path) result.paths.emplace_back(path);
    }
#else
    // WebKitGTK already loads these libraries. Keep GTK types out of the
    // desktop wrapper's public API, as with the window operations above.
    auto& api = gtk_window_api();
    if (!api.load()) {
        result.status = Result::Status::Unavailable;
        result.detail = "filesystem clipboard is unavailable";
        return result;
    }
    const auto atom = reinterpret_cast<void* (*)(const char*, int)>(dlsym(api.gdk, "gdk_atom_intern"));
    const auto clipboard_get = reinterpret_cast<void* (*)(void*)>(dlsym(api.gtk, "gtk_clipboard_get"));
    const auto read_uris = reinterpret_cast<char** (*)(void*)>(dlsym(api.gtk, "gtk_clipboard_wait_for_uris"));
    const auto filename = reinterpret_cast<char* (*)(const char*, char**, void**)>(dlsym(api.gtk, "g_filename_from_uri"));
    const auto free_string = reinterpret_cast<void (*)(void*)>(dlsym(api.gtk, "g_free"));
    const auto free_strings = reinterpret_cast<void (*)(char**)>(dlsym(api.gtk, "g_strfreev"));
    if (!atom || !clipboard_get || !read_uris || !filename || !free_string || !free_strings) {
        result.status = Result::Status::Unavailable;
        result.detail = "filesystem clipboard is unavailable";
        return result;
    }
    char** uris = read_uris(clipboard_get(atom("CLIPBOARD", 0)));
    if (uris) {
        for (char** uri = uris; *uri; ++uri) {
            char* path = filename(*uri, nullptr, nullptr);
            if (path) {
                result.paths.emplace_back(path);
                free_string(path);
            }
        }
        free_strings(uris);
    }
#endif
    if (result.paths.size() > acecode::kMaxClipboardFilesystemPaths) {
        result.status = Result::Status::TooMany;
        result.paths.clear();
        result.detail = "clipboard contains too many filesystem items";
    } else if (!result.paths.empty()) result.status = Result::Status::Success;
    return result;
#endif
}
void WebHost::request_quit() {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return;
    ::PostMessageW(hwnd, kRequestQuitMsg, 0, 0);
#else
#if !defined(__APPLE__)
#ifdef ACECODE_DEEPIN
    // gtk_window_close can destroy the widget before WebHost's destructor.
    // Release foreign handles and GTK signal owners while it is still alive.
    impl_->deepin_effects.reset();
    impl_->linux_scale.reset();
#endif
    auto window = impl_->w->window();
    g_linux_force_close = true;
    if (window.ok() && window.value()) {
        auto& api = gtk_window_api();
        if (api.load()) {
            api.window_close(window.value());
        }
    }
#endif
    impl_->w->terminate();
#endif
}
void WebHost::bind(const std::string& name, SyncHandler fn) {
    impl_->w->bind(name, [fn](const std::string& req) -> std::string {
        return fn(req);
    });
}
void WebHost::run() {
    impl_->w->run();
}

void* WebHost::native_window() const {
    // basic_result<void*>: ok()/value() 接口。失败时返回 nullptr 让 folder picker
    // 用 NULL parent(对 IFileOpenDialog 是合法的,弹窗 modal 关系会缺失)。
    auto r = impl_->w->window();
    return r.ok() ? r.value() : nullptr;
}

} // namespace acecode::desktop
