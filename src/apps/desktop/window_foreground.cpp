#include "window_foreground.hpp"

#ifdef _WIN32
#  include "platform/native_ui/notifications.hpp"

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace acecode::desktop {

bool show_window_in_foreground(void* native_window, const std::function<void()>& show) {
#ifdef _WIN32
    auto* hwnd = static_cast<HWND>(native_window);
    if (!hwnd || !::IsWindow(hwnd)) {
        if (show) show();
        return false;
    }

    const HWND foreground = ::GetForegroundWindow();
    const DWORD current_thread = ::GetCurrentThreadId();
    const DWORD foreground_thread =
        foreground ? ::GetWindowThreadProcessId(foreground, nullptr) : 0;
    // 并入输入队列后,激活切换会同步经过对方线程;对方无响应时不能并入,
    // 否则本线程会跟着卡住。
    const bool attached = foreground_thread && foreground_thread != current_thread &&
        !::IsHungAppWindow(foreground) &&
        ::AttachThreadInput(current_thread, foreground_thread, TRUE) != FALSE;
    if (show) show();
    ::BringWindowToTop(hwnd);
    ::SetForegroundWindow(hwnd);
    if (attached) ::AttachThreadInput(current_thread, foreground_thread, FALSE);
    if (::GetForegroundWindow() == hwnd) return true;

    // 置顶脉冲:拿不到输入焦点也保证窗口在最上层可见。
    return activate_notification_window(hwnd);
#else
    (void)native_window;
    if (show) show();
    return true;
#endif
}

} // namespace acecode::desktop
