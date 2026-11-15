#pragma once
#include "web_host.hpp"
#include "platform/clipboard.hpp"
#include <vector>
#if !defined(_WIN32) && !defined(__APPLE__)
#include "single_instance.hpp"
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <sys/socket.h>
#endif
#ifdef __APPLE__
#import <AppKit/AppKit.h>
#include <objc/runtime.h>
#endif

namespace acecode::desktop::detail {
#if !defined(_WIN32) && !defined(__APPLE__)
struct FocusSourceTraits {
    using handle_type = guint;
    static guint invalid() noexcept { return 0; }
    static bool valid(guint value) noexcept { return value != 0; }
    static void close(guint value) noexcept { g_source_remove(value); }
};
class LinuxInviteFocusReceiver {
public:
    LinuxInviteFocusReceiver(const LinuxInviteFocusReceiver&) = delete;
    LinuxInviteFocusReceiver& operator=(const LinuxInviteFocusReceiver&) = delete;
    LinuxInviteFocusReceiver(LinuxInviteFocusReceiver&&) = delete;
    LinuxInviteFocusReceiver& operator=(LinuxInviteFocusReceiver&&) = delete;
    // GUI-thread resource; source is removed before callback state or the
    // borrowed GTK window can be destroyed. No delivery runs off the GUI thread.
    LinuxInviteFocusReceiver(void* window, std::function<void()> callback)
        : window_(window), callback_(std::move(callback)), socket_(open_desktop_focus_socket()) {
        if (socket_ && window_) source_.reset(g_unix_fd_add(socket_.get(), G_IO_IN,
            [](gint fd, GIOCondition, gpointer data) -> gboolean {
                auto& self = *static_cast<LinuxInviteFocusReceiver*>(data);
                char buffer[64];
                while (::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {}
                gtk_window_present(GTK_WINDOW(self.window_));
                if (self.callback_) self.callback_();
                return G_SOURCE_CONTINUE;
            }, this));
    }
private:
    void* window_; // Borrowed until source_ is removed.
    std::function<void()> callback_;
    platform::UniqueFd socket_;
    platform::UniqueResource<FocusSourceTraits> source_;
};
#endif
// GUI-thread callbacks. WebHost's destructor clears every registration.
inline WebHost::ClipboardChangedHandler g_clipboard_changed_handler;
inline WebHost::OpenInviteHandler g_open_invite_handler;
inline std::vector<std::string> g_pending_native_invites;

#ifdef __APPLE__
inline void mac_open_invite_event(id, SEL, NSAppleEventDescriptor* event, NSAppleEventDescriptor*) {
    NSString* value = [[event paramDescriptorForKeyword:'----'] stringValue];
    if (!value) return;
    const std::string link = value.UTF8String ?: "";
    if (link.rfind("acecode://connect/", 0) != 0 || link.size() > 8192) return;
    if (g_open_invite_handler) g_open_invite_handler(link);
    else g_pending_native_invites.push_back(link);
}

inline void install_mac_invite_events() {
    id delegate = NSApp.delegate;
    if (!delegate) return;
    const SEL selector = sel_registerName("acecodeOpenInvite:withReply:");
    class_addMethod(object_getClass(delegate), selector, reinterpret_cast<IMP>(mac_open_invite_event), "v@:@@");
    [[NSAppleEventManager sharedAppleEventManager] setEventHandler:delegate andSelector:selector
        forEventClass:'GURL' andEventID:'GURL'];
}

#endif
inline void set_clipboard_changed_handler(WebHost::ClipboardChangedHandler handler) {
    g_clipboard_changed_handler = std::move(handler);
}
inline void set_open_invite_handler(WebHost::OpenInviteHandler handler) {
    g_open_invite_handler = std::move(handler);
    if (g_open_invite_handler) {
        const auto pending = std::move(g_pending_native_invites);
        g_pending_native_invites.clear();
        for (const auto& link : pending) g_open_invite_handler(link);
    }
}
inline void check_clipboard_invitation() {
#ifdef __APPLE__
    NSPasteboard* pasteboard = NSPasteboard.generalPasteboard;
    const auto read = ^{
        if (!g_clipboard_changed_handler) return;
        if ([[pasteboard types] containsObject:@"org.nspasteboard.ConcealedType"] ||
            [[pasteboard types] containsObject:@"org.nspasteboard.TransientType"]) return;
        NSString* text = [pasteboard stringForType:NSPasteboardTypeString];
        if (!text) return;
        if (text.length > 8192) text = [text substringToIndex:8192];
        const auto link = acecode::extract_clipboard_invitation(text.UTF8String ?: "");
        if (!link.empty()) g_clipboard_changed_handler(true, link);
    };
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150400
    if (@available(macOS 15.4, *)) {
        if (pasteboard.accessBehavior == NSPasteboardAccessBehaviorAlwaysDeny) return;
        const NSInteger change = pasteboard.changeCount;
        [pasteboard detectPatternsForPatterns:[NSSet setWithObject:NSPasteboardDetectionPatternProbableWebURL]
            completionHandler:^(NSSet* patterns, NSError* error) {
                if (error || patterns.count == 0) return;
                dispatch_async(dispatch_get_main_queue(), ^{ if (pasteboard.changeCount == change) read(); });
            }];
        return;
    }
#endif
    read();
#endif
}

inline void notify_invite_clipboard_change(bool foreground) {
    if (g_clipboard_changed_handler) g_clipboard_changed_handler(foreground, {});
}
inline void clear_invite_handlers() {
    g_clipboard_changed_handler = {};
    g_open_invite_handler = {};
    g_pending_native_invites.clear();
#ifdef __APPLE__
    [[NSAppleEventManager sharedAppleEventManager] removeEventHandlerForEventClass:'GURL' andEventID:'GURL'];
#endif
}
} // namespace acecode::desktop::detail
