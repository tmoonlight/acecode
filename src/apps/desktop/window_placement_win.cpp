#include "window_placement_win.hpp"

#ifdef _WIN32
#include "web_host_chrome_win.hpp"

namespace acecode::desktop::detail {

WindowBoundsMemory g_window_bounds;

namespace {

// 与 frameless_nc_calc 同口径:左 / 右 / 下各留 frame + padding,顶边为 0。
WindowFrameInsets invisible_frame_insets(UINT dpi) {
    if (dpi == 0) dpi = 96;
    const int padding = ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const int frame_x = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi) + padding;
    const int frame_y = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi) + padding;
    return {frame_x, 0, frame_x, frame_y};
}

DesktopWindowMonitor desktop_monitor(const RECT& work_area, UINT dpi) {
    return {{work_area.left, work_area.top, work_area.right, work_area.bottom},
            static_cast<int>(dpi)};
}

} // namespace

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

std::optional<SavedDesktopWindowBounds> capture_window_bounds(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) return std::nullopt;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!::GetWindowPlacement(hwnd, &placement)) return std::nullopt;

    const bool minimized = ::IsIconic(hwnd) != FALSE;
    const bool maximized = ::IsZoomed(hwnd) != FALSE;
    // 普通状态直接取窗口矩形:贴边分屏后 rcNormalPosition 还是分屏前的旧值。
    RECT rect{};
    if (minimized || maximized) {
        // rcNormalPosition 是工作区坐标(以所在显示器工作区左上角为原点),换回屏幕坐标。
        rect = placement.rcNormalPosition;
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
            ::OffsetRect(&rect, info.rcWork.left - info.rcMonitor.left,
                         info.rcWork.top - info.rcMonitor.top);
        }
    } else if (!::GetWindowRect(hwnd, &rect)) {
        return std::nullopt;
    }

    UINT dpi = ::GetDpiForWindow(hwnd);
    if (dpi == 0) dpi = 96;
    if (g_window_shape.chamfer) {
        // 切角时外框就是可见边框;补回隐形缩放边,存成普通边框口径。
        const WindowFrameInsets insets = invisible_frame_insets(dpi);
        rect.left -= insets.left;
        rect.right += insets.right;
        rect.bottom += insets.bottom;
    }
    if (rect.right <= rect.left || rect.bottom <= rect.top) return std::nullopt;

    SavedDesktopWindowBounds bounds;
    bounds.rect = {rect.left, rect.top, rect.right, rect.bottom};
    bounds.dpi = static_cast<int>(dpi);
    bounds.maximized =
        maximized || (minimized && (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);
    return bounds;
}

bool apply_saved_window_bounds(HWND hwnd,
                               const SavedDesktopWindowBounds& saved,
                               const RECT& fallback_work_area,
                               UINT fallback_dpi) {
    if (!hwnd) return false;
    const RECT saved_rect{saved.rect.left, saved.rect.top, saved.rect.right, saved.rect.bottom};
    // 保存的位置已不在任何显示器上(显示器拔掉了)时 DPI 记 0,一定走回退。
    DesktopWindowMonitor saved_monitor{{0, 0, 0, 0}, 0};
    if (HMONITOR monitor = ::MonitorFromRect(&saved_rect, MONITOR_DEFAULTTONULL)) {
        saved_monitor = desktop_monitor(monitor_work_rect(monitor), monitor_dpi(monitor));
    }
    const WindowRect placed = place_saved_desktop_window(
        saved,
        invisible_frame_insets(static_cast<UINT>(saved.dpi)),
        saved_monitor,
        desktop_monitor(fallback_work_area, fallback_dpi));

    RECT target{placed.left, placed.top, placed.right, placed.bottom};
    if (g_window_shape.chamfer) {
        // 页面已切到切角主题:外框要收掉隐形缩放边,否则每次启动可见窗口都大一圈。
        const WindowFrameInsets insets = invisible_frame_insets(
            monitor_dpi(::MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST)));
        target.left += insets.left;
        target.right -= insets.right;
        target.bottom -= insets.bottom;
    }
    // 跨 DPI 移动时 WM_DPICHANGED 会按系统建议矩形改尺寸;落到目标显示器后再设一次。
    for (int attempt = 0; attempt < 2; ++attempt) {
        ::SetWindowPos(hwnd, nullptr, target.left, target.top,
                       target.right - target.left, target.bottom - target.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        RECT actual{};
        if (!::GetWindowRect(hwnd, &actual) || ::EqualRect(&actual, &target)) break;
    }
    return saved.maximized;
}

} // namespace acecode::desktop::detail
#endif
