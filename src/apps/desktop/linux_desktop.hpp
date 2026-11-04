#pragma once

#include "platform/native_ui/folder_picker.hpp"
#include "window_size.hpp"

#include <optional>
#include <string>

namespace acecode::desktop {

// GTK uses logical coordinates for both monitor work areas and window sizes.
WindowRect linux_active_work_area();
// 返回摆放的目标矩形(请求值,窗口管理器可能稍后才生效)。
WindowRect center_linux_window(void* window, WindowRect work_area);
// 主窗口首次显示:有保存值就按保存值摆放(X11 完整可见时原位恢复,Wayland 只恢复
// 尺寸并居中),否则照旧居中;之后开始记录普通状态的位置和尺寸。
void place_linux_window_on_first_show(void* window,
                                      const std::optional<SavedDesktopWindowBounds>& saved);
// 最后一次记下的主窗口;首次显示之前为空。
std::optional<SavedDesktopWindowBounds> linux_window_bounds();
FolderPickOutcome pick_linux_folder(void* parent);
void configure_linux_native_services(void* webview_widget);
// Origin of the page native code navigated to; only it may use the microphone.
void set_linux_trusted_page_origin(void* webview_widget, const std::string& origin);

} // namespace acecode::desktop
