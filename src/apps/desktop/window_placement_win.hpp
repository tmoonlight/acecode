#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "window_size.hpp"

#include <optional>

namespace acecode::desktop::detail {

UINT monitor_dpi(HMONITOR monitor);
RECT monitor_work_rect(HMONITOR monitor);

// 主窗口位置记忆。只在宿主 GUI 线程读写:首次显示之后 shown 才为 true
// (离屏启动期间的位置不能记),WM_DESTROY 时把当时的窗口抓进 on_destroy。
struct WindowBoundsMemory {
    bool shown = false;
    std::optional<SavedDesktopWindowBounds> on_destroy;
};
extern WindowBoundsMemory g_window_bounds;

// 读窗口的普通状态外框(最大化 / 最小化时取还原后的矩形),换算成普通边框口径。
std::optional<SavedDesktopWindowBounds> capture_window_bounds(HWND hwnd);

// 首次显示前按保存值摆放窗口;仍完整可见就原样恢复,否则在 fallback_work_area
// 上按原有逻辑收进安全区并居中。返回是否应以最大化显示。
bool apply_saved_window_bounds(HWND hwnd,
                               const SavedDesktopWindowBounds& saved,
                               const RECT& fallback_work_area,
                               UINT fallback_dpi);

} // namespace acecode::desktop::detail
#endif
