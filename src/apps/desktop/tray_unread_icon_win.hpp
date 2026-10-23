#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <memory>
#include <type_traits>

namespace acecode::desktop {

struct TrayIconDeleter {
    void operator()(HICON icon) const { if (icon) ::DestroyIcon(icon); }
};
using UniqueTrayIcon = std::unique_ptr<std::remove_pointer_t<HICON>, TrayIconDeleter>;

// Original icon is borrowed for this call; the returned icon owns its pixels.
UniqueTrayIcon create_tray_unread_icon(HICON original, int size);

// Owned icon frames remain alive while the notification area uses them.
// All methods run on the Desktop UI thread.
class TrayUnreadIconState {
public:
    bool refresh(NOTIFYICONDATAW& notification, bool unread, bool add = false);
    bool unread() const { return unread_; }
    void clear() { *this = TrayUnreadIconState{}; }

private:
    UniqueTrayIcon original_;
    UniqueTrayIcon marked_;
    bool unread_ = false;
};

} // namespace acecode::desktop
#endif
