#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

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

// macOS(点)与 Linux(GTK 逻辑像素)的窗口和工作区都用与缩放无关的逻辑单位,
// 保存时 dpi 恒记这个值,显示器也按它比较,缩放变化不会触发尺寸换算。
inline constexpr int kLogicalDesktopWindowDpi = 96;

// 与 rect 重叠面积最大的显示器;一个都不重叠(显示器已拔掉)时为空。
inline std::optional<DesktopWindowMonitor> desktop_monitor_for_rect(
    WindowRect rect, const std::vector<DesktopWindowMonitor>& monitors) {
    std::optional<DesktopWindowMonitor> best;
    long long best_area = 0;
    for (const auto& monitor : monitors) {
        const long long width =
            std::min<long long>(rect.right, monitor.work_area.right) -
            std::max<long long>(rect.left, monitor.work_area.left);
        const long long height =
            std::min<long long>(rect.bottom, monitor.work_area.bottom) -
            std::max<long long>(rect.top, monitor.work_area.top);
        if (width <= 0 || height <= 0 || width * height <= best_area) continue;
        best_area = width * height;
        best = monitor;
    }
    return best;
}

struct DesktopWindowPlacement {
    WindowRect rect;
    // true = 原位恢复;false = 回退到「保存的尺寸收进安全区并居中」。
    bool restored = false;
};

// 逻辑坐标平台的首次摆放。没有隐形缩放边;monitors 为空表示窗口位置读不到也
// 设不了(Wayland),这时只沿用保存的尺寸,在回退工作区上居中。
inline DesktopWindowPlacement place_saved_logical_desktop_window(
    const SavedDesktopWindowBounds& saved,
    const std::vector<DesktopWindowMonitor>& monitors,
    WindowRect fallback_work_area) {
    const auto saved_monitor = desktop_monitor_for_rect(saved.rect, monitors);
    if (saved_monitor && saved.dpi == saved_monitor->dpi &&
        desktop_window_visible_in_work_area(saved.rect, {}, saved_monitor->work_area)) {
        return {saved.rect, true};
    }
    // DPI 为 0 的空显示器让 place_saved_desktop_window 一定走回退分支。
    return {place_saved_desktop_window(saved, {}, {{0, 0, 0, 0}, 0},
                                       {fallback_work_area, kLogicalDesktopWindowDpi}),
            false};
}

// AppKit 的 frame:原点在主屏(菜单栏所在屏)左下角,y 向上,单位是点。
struct CocoaWindowFrame {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

inline int round_window_coordinate(double value) {
    const double limited = std::min<double>(std::numeric_limits<int>::max(),
                                            std::max<double>(std::numeric_limits<int>::min(), value));
    return static_cast<int>(std::lround(limited));
}

// 换成与 Windows 相同的左上原点、y 向下口径;primary_screen_top 是主屏 frame 的 maxY。
// 四条边各自取整,来回换算不会累积误差。
inline WindowRect top_left_rect_from_cocoa_frame(CocoaWindowFrame frame,
                                                 double primary_screen_top) {
    return {
        round_window_coordinate(frame.x),
        round_window_coordinate(primary_screen_top - (frame.y + frame.height)),
        round_window_coordinate(frame.x + frame.width),
        round_window_coordinate(primary_screen_top - frame.y),
    };
}

inline CocoaWindowFrame cocoa_frame_from_top_left_rect(WindowRect rect,
                                                       double primary_screen_top) {
    return {
        static_cast<double>(rect.left),
        primary_screen_top - static_cast<double>(rect.bottom),
        static_cast<double>(normalized_window_extent(rect.left, rect.right)),
        static_cast<double>(normalized_window_extent(rect.top, rect.bottom)),
    };
}

} // namespace acecode::desktop
