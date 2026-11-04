#include "window_placement_mac.hpp"

#ifdef __APPLE__
#include "window_bounds_state.hpp"

#include <initializer_list>
#include <vector>

// 与 web_host.cpp 同样按 Objective-C++ 编译、不开 ARC:窗口对象由这里 retain,
// 关窗后(NSWindow 关闭即释放)退出时仍能安全读取。
namespace acecode::desktop::detail {

namespace {

double primary_screen_top() {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    return screens.count > 0 ? NSMaxY([screens[0] frame]) : 0.0;
}

WindowRect top_left_rect(NSRect frame, double primary_top) {
    return top_left_rect_from_cocoa_frame(
        {frame.origin.x, frame.origin.y, frame.size.width, frame.size.height}, primary_top);
}

bool fullscreen(NSWindow* window) {
    return ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
}

} // namespace

MacWindowBoundsMemory::MacWindowBoundsMemory(NSWindow* window)
    : window_([window retain]) {
    if (!window_) return;
    MacWindowBoundsMemory* memory = this;
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    // queue 传 nil:在发出通知的主线程上同步执行,窗口关闭 / 进程退出之前一定跑完。
    auto observe = ^id(NSNotificationName name, bool entering_fullscreen) {
        return [center addObserverForName:name
                                   object:memory->window_
                                    queue:nil
                               usingBlock:^(__unused NSNotification* note) {
                                   memory->remember_now(entering_fullscreen);
                               }];
    };
    will_close_ = observe(NSWindowWillCloseNotification, false);
    // 进入全屏的通知到达时 styleMask 可能已带全屏位,frame 却还是进入前的。
    will_enter_fullscreen_ = observe(NSWindowWillEnterFullScreenNotification, true);
    will_miniaturize_ = observe(NSWindowWillMiniaturizeNotification, false);
    // Dock「退出」、注销走 [NSApp terminate:],进程直接退出,main 里 run() 之后的
    // 保存不会执行,只能在这里写。
    will_terminate_ = [center addObserverForName:NSApplicationWillTerminateNotification
                                          object:nil
                                           queue:nil
                                      usingBlock:^(__unused NSNotification* note) {
                                          if (const auto bounds = memory->capture()) {
                                              write_desktop_window_bounds(*bounds);
                                          }
                                      }];
}

MacWindowBoundsMemory::~MacWindowBoundsMemory() {
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    for (id observer : {will_close_, will_enter_fullscreen_, will_miniaturize_, will_terminate_}) {
        if (observer) [center removeObserver:observer];
    }
    [window_ release];
}

void MacWindowBoundsMemory::restore(const SavedDesktopWindowBounds& saved) {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    if (!window_ || screens.count == 0) return;
    const double primary_top = primary_screen_top();
    std::vector<DesktopWindowMonitor> monitors;
    for (NSScreen* screen in screens) {
        monitors.push_back({top_left_rect([screen visibleFrame], primary_top),
                            kLogicalDesktopWindowDpi});
    }
    NSScreen* fallback = [window_ screen] ?: [NSScreen mainScreen] ?: screens[0];
    const DesktopWindowPlacement placement = place_saved_logical_desktop_window(
        saved, monitors, top_left_rect([fallback visibleFrame], primary_top));
    const CocoaWindowFrame frame = cocoa_frame_from_top_left_rect(placement.rect, primary_top);
    [window_ setFrame:NSMakeRect(frame.x, frame.y, frame.width, frame.height)
              display:YES
              animate:NO];
    // 回退时只沿用尺寸,位置交回原来的居中逻辑。
    if (!placement.restored) [window_ center];
    if (saved.maximized && ![window_ isZoomed]) {
        remember();
        [window_ zoom:nil];
    }
}

void MacWindowBoundsMemory::remember() {
    remember_now(false);
}

void MacWindowBoundsMemory::remember_now(bool entering_fullscreen) {
    if (!readable(entering_fullscreen)) return;
    if (![window_ isZoomed]) {
        pre_zoom_ = top_left_rect([window_ frame], primary_screen_top());
    }
    if (auto live = read_live(entering_fullscreen)) remembered_ = live;
}

std::optional<SavedDesktopWindowBounds> MacWindowBoundsMemory::capture() const {
    if (auto live = read_live(false)) return live;
    return remembered_;
}

bool MacWindowBoundsMemory::readable(bool entering_fullscreen) const {
    // 隐藏到托盘、最小化、全屏或已关闭时 frame / isZoomed 不可信,交给 remembered_。
    return window_ && [window_ isVisible] && ![window_ isMiniaturized] &&
           (entering_fullscreen || !fullscreen(window_)) && [[NSScreen screens] count] > 0;
}

std::optional<SavedDesktopWindowBounds> MacWindowBoundsMemory::read_live(
    bool entering_fullscreen) const {
    if (!readable(entering_fullscreen)) return std::nullopt;
    SavedDesktopWindowBounds bounds;
    bounds.maximized = [window_ isZoomed] == YES;
    bounds.rect = bounds.maximized && pre_zoom_
        ? *pre_zoom_
        : top_left_rect([window_ frame], primary_screen_top());
    bounds.dpi = kLogicalDesktopWindowDpi;
    if (bounds.rect.right <= bounds.rect.left || bounds.rect.bottom <= bounds.rect.top) {
        return std::nullopt;
    }
    return bounds;
}

} // namespace acecode::desktop::detail
#endif
