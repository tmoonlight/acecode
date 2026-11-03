#pragma once

#include <algorithm>
#include <limits>

namespace acecode::desktop {

struct WindowSize {
    int width = 1;
    int height = 1;
};

struct WindowRect {
    int left = 0;
    int top = 0;
    int right = 1;
    int bottom = 1;
};

inline constexpr int kDesktopWindowSafeHorizontalMarginDip = 40;
inline constexpr int kDesktopWindowSafeVerticalMarginDip = 60;

// Clamp an outer native window size to a positive usable display area.
// Degenerate inputs are normalized to one pixel so callers never hand an
// invalid zero/negative dimension to a platform window API.
inline WindowSize clamp_window_size_to_work_area(WindowSize requested,
                                                  WindowSize work_area) {
    const int max_width = std::max(1, work_area.width);
    const int max_height = std::max(1, work_area.height);
    return {
        std::min(std::max(1, requested.width), max_width),
        std::min(std::max(1, requested.height), max_height),
    };
}

inline int window_dip_to_pixels(int value, int dpi) {
    if (value <= 0) return 0;
    const int effective_dpi = dpi > 0 ? dpi : 96;
    const long long scaled =
        (static_cast<long long>(value) * static_cast<long long>(effective_dpi)) / 96;
    return static_cast<int>(std::min<long long>(
        std::numeric_limits<int>::max(), std::max<long long>(0, scaled)));
}

// Fit the preferred initial outer-window size inside the usable display area
// while preserving visible breathing room around the first screen.
inline WindowSize fit_desktop_window_to_safe_work_area(WindowSize requested,
                                                        WindowSize work_area,
                                                        int dpi) {
    const int horizontal_margin =
        window_dip_to_pixels(kDesktopWindowSafeHorizontalMarginDip, dpi);
    const int vertical_margin =
        window_dip_to_pixels(kDesktopWindowSafeVerticalMarginDip, dpi);
    const long long safe_width = std::max<long long>(
        1,
        static_cast<long long>(work_area.width) -
            2LL * static_cast<long long>(horizontal_margin));
    const long long safe_height = std::max<long long>(
        1,
        static_cast<long long>(work_area.height) -
            2LL * static_cast<long long>(vertical_margin));
    return clamp_window_size_to_work_area(
        requested,
        {static_cast<int>(safe_width), static_cast<int>(safe_height)});
}

inline int normalized_window_extent(int start, int end) {
    const long long extent =
        static_cast<long long>(end) - static_cast<long long>(start);
    return static_cast<int>(std::min<long long>(
        std::numeric_limits<int>::max(), std::max<long long>(1, extent)));
}

// Fit an already DPI-scaled native outer-window size into an absolute work-area
// rectangle and center it. This is intentionally applied after platform WebView
// sizing because that layer can scale a logical request and add its own frame.
inline WindowRect fit_centered_desktop_window_rect_to_safe_work_area(
    WindowSize actual_outer_size,
    WindowRect work_area,
    int dpi) {
    const int work_width = normalized_window_extent(work_area.left, work_area.right);
    const int work_height = normalized_window_extent(work_area.top, work_area.bottom);
    const WindowSize fitted = fit_desktop_window_to_safe_work_area(
        actual_outer_size, {work_width, work_height}, dpi);

    const long long left = static_cast<long long>(work_area.left) +
        (static_cast<long long>(work_width) - fitted.width) / 2;
    const long long top = static_cast<long long>(work_area.top) +
        (static_cast<long long>(work_height) - fitted.height) / 2;
    auto to_int = [](long long value) {
        return static_cast<int>(std::min<long long>(
            std::numeric_limits<int>::max(),
            std::max<long long>(std::numeric_limits<int>::min(), value)));
    };
    return {
        to_int(left),
        to_int(top),
        to_int(left + fitted.width),
        to_int(top + fitted.height),
    };
}

// 上次退出时的窗口:非最大化时的外框(屏幕物理像素,按普通边框口径,即含
// 隐形缩放边、不含切角主题的收缩)+ 当时窗口所在显示器的 DPI + 是否最大化。
struct SavedDesktopWindowBounds {
    WindowRect rect;
    int dpi = 96;
    bool maximized = false;
};

// 外框比用户看得见的窗口多出的隐形缩放边(Windows 无标题栏窗口左/右/下各一条)。
struct WindowFrameInsets {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct DesktopWindowMonitor {
    WindowRect work_area;
    int dpi = 96;
};

// 保存的外框去掉隐形缩放边后完全落在工作区里,才算没超出可视范围。
inline bool desktop_window_visible_in_work_area(WindowRect outer,
                                                WindowFrameInsets invisible_frame,
                                                WindowRect work_area) {
    const long long left = static_cast<long long>(outer.left) + invisible_frame.left;
    const long long top = static_cast<long long>(outer.top) + invisible_frame.top;
    const long long right = static_cast<long long>(outer.right) - invisible_frame.right;
    const long long bottom = static_cast<long long>(outer.bottom) - invisible_frame.bottom;
    if (right <= left || bottom <= top) return false;
    if (work_area.right <= work_area.left || work_area.bottom <= work_area.top) return false;
    return left >= work_area.left && top >= work_area.top &&
           right <= work_area.right && bottom <= work_area.bottom;
}

// 决定首次显示时的窗口外框。保存的窗口仍完整可见、且所在显示器 DPI 没变 →
// 原样恢复位置和尺寸;否则走启动时原有的逻辑:把保存的尺寸按回退显示器的
// DPI 换算后,收进安全工作区并居中。
inline WindowRect place_saved_desktop_window(const SavedDesktopWindowBounds& saved,
                                             WindowFrameInsets invisible_frame,
                                             DesktopWindowMonitor saved_monitor,
                                             DesktopWindowMonitor fallback_monitor) {
    if (saved.dpi > 0 && saved.dpi == saved_monitor.dpi &&
        desktop_window_visible_in_work_area(saved.rect, invisible_frame,
                                            saved_monitor.work_area)) {
        return saved.rect;
    }
    const int saved_dpi = saved.dpi > 0 ? saved.dpi : 96;
    const int target_dpi = fallback_monitor.dpi > 0 ? fallback_monitor.dpi : 96;
    auto scale = [&](int extent) {
        const long long scaled = static_cast<long long>(extent) * target_dpi / saved_dpi;
        return static_cast<int>(std::min<long long>(
            std::numeric_limits<int>::max(), std::max<long long>(1, scaled)));
    };
    const WindowSize size{
        scale(normalized_window_extent(saved.rect.left, saved.rect.right)),
        scale(normalized_window_extent(saved.rect.top, saved.rect.bottom)),
    };
    return fit_centered_desktop_window_rect_to_safe_work_area(
        size, fallback_monitor.work_area, target_dpi);
}

} // namespace acecode::desktop
