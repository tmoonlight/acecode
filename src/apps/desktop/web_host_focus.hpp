#pragma once

#include <functional>
#include <webview/webview.h>

#ifdef __APPLE__
#import <AppKit/AppKit.h>
#elif !defined(_WIN32)
#include <gtk/gtk.h>
#endif

namespace acecode::desktop::detail {

// Single GUI-thread host callback, cleared before the native window is destroyed.
inline std::function<void()> window_focus_handler;

inline bool focus_active_webview(webview::webview& host) {
    auto window = host.window();
    if (!window.ok() || !window.value()) return false;
#ifdef _WIN32
    auto hwnd = static_cast<HWND>(window.value());
    if (::GetForegroundWindow() != hwnd || ::IsIconic(hwnd) || !::IsWindowVisible(hwnd)) return false;
    auto controller_result = host.browser_controller();
    if (!controller_result.ok()) return false;
    auto* controller = static_cast<ICoreWebView2Controller*>(controller_result.value());
    return controller && SUCCEEDED(controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC));
#else
    auto widget = host.widget();
    if (!widget.ok() || !widget.value()) return false;
#ifdef __APPLE__
    auto* native_window = static_cast<NSWindow*>(window.value());
    if (![native_window isKeyWindow] || ![native_window isVisible] || [native_window isMiniaturized]) return false;
    return [native_window makeFirstResponder:static_cast<NSView*>(widget.value())] == YES;
#else
    if (!gtk_window_is_active(GTK_WINDOW(window.value())) ||
        !gtk_widget_get_mapped(GTK_WIDGET(window.value()))) return false;
    gtk_widget_grab_focus(GTK_WIDGET(widget.value()));
    return gtk_widget_has_focus(GTK_WIDGET(widget.value())) != FALSE;
#endif
#endif
}

#ifdef __APPLE__
inline NSWindow* mac_window_from_host(webview::webview& host) {
    auto window = host.window();
    return window.ok() ? static_cast<NSWindow*>(window.value()) : nil;
}

class MacWindowFocusObservers {
public:
    explicit MacWindowFocusObservers(webview::webview& host) {
        auto* window = mac_window_from_host(host);
        if (!window) return;
        key_ = observe(window, NSWindowDidBecomeKeyNotification);
        restore_ = observe(window, NSWindowDidDeminiaturizeNotification);
    }
    ~MacWindowFocusObservers() {
        if (key_) [[NSNotificationCenter defaultCenter] removeObserver:key_];
        if (restore_) [[NSNotificationCenter defaultCenter] removeObserver:restore_];
    }
    MacWindowFocusObservers(const MacWindowFocusObservers&) = delete;
    MacWindowFocusObservers& operator=(const MacWindowFocusObservers&) = delete;

private:
    static id observe(NSWindow* window, NSNotificationName name) {
        return [[NSNotificationCenter defaultCenter]
            addObserverForName:name object:window queue:[NSOperationQueue mainQueue]
            usingBlock:^(__unused NSNotification* note) {
                if ([window isKeyWindow] && window_focus_handler) window_focus_handler();
            }];
    }
    id key_ = nil;
    id restore_ = nil;
};
#elif !defined(_WIN32)
inline void install_linux_window_focus_handler(webview::webview& host) {
    auto window = host.window();
    if (!window.ok() || !window.value()) return;
    // The signal and its stateless callback are destroyed with the GTK window.
    g_signal_connect(window.value(), "focus-in-event",
        G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer) -> gboolean {
            if (window_focus_handler) window_focus_handler();
            return FALSE;
        }), nullptr);
}
#endif

} // namespace acecode::desktop::detail
