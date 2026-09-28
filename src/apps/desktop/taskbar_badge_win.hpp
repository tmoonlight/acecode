#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "taskbar_badge.hpp"

namespace acecode::desktop {

// The caller owns the returned icon. The original application icon is borrowed.
HICON create_taskbar_badged_icon(HICON original, int size, const TaskbarBadge& badge);

class WindowsTaskbarBadge {
public:
    explicit WindowsTaskbarBadge(HWND window);
    ~WindowsTaskbarBadge();
    WindowsTaskbarBadge(const WindowsTaskbarBadge&) = delete;
    WindowsTaskbarBadge& operator=(const WindowsTaskbarBadge&) = delete;

    bool set(const TaskbarBadge& badge);

private:
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    bool refresh();
    void apply_icons(HICON large_icon, HICON small_icon);
    void release_badged_icons();

    HWND window_ = nullptr;
    HICON original_large_ = nullptr;
    HICON original_small_ = nullptr;
    HICON badged_large_ = nullptr;
    HICON badged_small_ = nullptr;
    TaskbarBadge badge_;
    UINT taskbar_created_ = 0;
};

} // namespace acecode::desktop
#endif
