#pragma once

#ifdef __APPLE__
#import <AppKit/AppKit.h>

#include "window_size.hpp"

#include <optional>

namespace acecode::desktop::detail {

// 主窗口位置记忆(macOS)。set_size 把窗口摆好之后创建,只在主线程使用。
// 保存口径:点,左上原点(主屏左上角为原点),dpi 恒为 kLogicalDesktopWindowDpi;
// 缩放(zoom)状态下保存缩放前的 frame 并记 maximized。
class MacWindowBoundsMemory {
public:
    explicit MacWindowBoundsMemory(NSWindow* window);
    ~MacWindowBoundsMemory();
    MacWindowBoundsMemory(const MacWindowBoundsMemory&) = delete;
    MacWindowBoundsMemory& operator=(const MacWindowBoundsMemory&) = delete;

    // 仍完整落在某块屏的可见区域内就原位恢复,否则沿用保存的尺寸并照旧 [window center]。
    void restore(const SavedDesktopWindowBounds& saved);
    // 缩放、隐藏到托盘之前调用:记下此刻的窗口,之后窗口不可见或已缩放时用它。
    void remember();
    // 退出时的窗口:能直接读就读当前窗口,否则用最后一次记下的值。
    std::optional<SavedDesktopWindowBounds> capture() const;

private:
    void remember_now(bool entering_fullscreen);
    bool readable(bool entering_fullscreen) const;
    std::optional<SavedDesktopWindowBounds> read_live(bool entering_fullscreen) const;

    NSWindow* window_ = nil;
    std::optional<WindowRect> pre_zoom_;
    std::optional<SavedDesktopWindowBounds> remembered_;
    id will_close_ = nil;
    id will_enter_fullscreen_ = nil;
    id will_miniaturize_ = nil;
    id will_terminate_ = nil;
};

} // namespace acecode::desktop::detail
#endif
