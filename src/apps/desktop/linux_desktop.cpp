#include "linux_desktop.hpp"

#include "microphone_permission.hpp"
#include "platform/native_ui/strings.hpp"

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <vector>

// X11 头文件定义了 None / Status 等宏,放在所有 ACECode 头文件之后。
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

namespace acecode::desktop {
namespace {
constexpr const char* kTrustedOriginKey = "acecode-microphone-origin";

// 窗口管理器对 gtk_window_move 的理解不一:按 ICCCM 是框架左上角,也就是
// gtk_window_get_position 读回的值;WSLg 的 Weston XWM 却把客户区放在那里,框架
// 另带阴影偏移。首次摆放后看哪个读回值等于请求的位置,之后按同一口径记,
// 下次 move 才回到原处。
enum class LinuxWindowPositionSource { Frame, ClientOrigin };
// 摆放迟迟没生效(窗口管理器另行摆放或用户立刻拖动)时,超过这么多次采样就按框架口径记。
constexpr int kMaxPositionCalibrationSamples = 10;

// 主窗口位置记忆。只在 GTK 主线程读写;窗口销毁后 window 置空,保留最后一次记下的值。
struct LinuxWindowBoundsMemory {
    GtkWindow* window = nullptr;
    // 只有 X11(含 XWayland)能读写窗口位置;Wayland 由合成器摆放,只记尺寸。
    bool positionable = false;
    guint sample_source = 0;
    std::optional<WindowRect> normal;
    bool maximized = false;
    // 首次摆放请求的位置;口径定下来之前不采样,免得把摆放前的旧位置记下。
    std::optional<WindowRect> requested;
    int calibration_samples = 0;
    LinuxWindowPositionSource source = LinuxWindowPositionSource::Frame;
};
LinuxWindowBoundsMemory g_window_bounds;

// 返回 false 表示摆放还没生效,这次不记。
bool calibrate_linux_window_position(GdkWindow* gdk_window, int* x, int* y) {
    auto& memory = g_window_bounds;
    int frame_x = 0;
    int frame_y = 0;
    int origin_x = 0;
    int origin_y = 0;
    gtk_window_get_position(memory.window, &frame_x, &frame_y);
    gdk_window_get_origin(gdk_window, &origin_x, &origin_y);
    if (memory.requested) {
        const WindowRect target = *memory.requested;
        if (frame_x == target.left && frame_y == target.top) {
            memory.source = LinuxWindowPositionSource::Frame;
        } else if (origin_x == target.left && origin_y == target.top) {
            memory.source = LinuxWindowPositionSource::ClientOrigin;
        } else if (++memory.calibration_samples < kMaxPositionCalibrationSamples) {
            return false;
        }
        memory.requested.reset();
    }
    const bool frame = memory.source == LinuxWindowPositionSource::Frame;
    *x = frame ? frame_x : origin_x;
    *y = frame ? frame_y : origin_y;
    return true;
}

bool linux_display_supports_window_position(GdkDisplay* display) {
#ifdef GDK_WINDOWING_X11
    return display && GDK_IS_X11_DISPLAY(display);
#else
    (void)display;
    return false;
#endif
}

// 隐藏到托盘、最小化、全屏期间不记,保留之前的普通状态;最大化时只更新标记,
// 普通状态的位置尺寸留给还原后用。贴边平铺按实际矩形记。
void sample_linux_window_bounds() {
    auto& memory = g_window_bounds;
    if (!memory.window || !gtk_widget_get_visible(GTK_WIDGET(memory.window))) return;
    GdkWindow* gdk_window = gtk_widget_get_window(GTK_WIDGET(memory.window));
    if (!gdk_window) return;
    const GdkWindowState state = gdk_window_get_state(gdk_window);
    if (state & (GDK_WINDOW_STATE_WITHDRAWN | GDK_WINDOW_STATE_ICONIFIED |
                 GDK_WINDOW_STATE_FULLSCREEN)) {
        return;
    }
    memory.maximized = (state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    if (memory.maximized) return;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (memory.positionable) {
        if (!calibrate_linux_window_position(gdk_window, &x, &y)) return;
    } else if (memory.normal) {
        // Wayland 读回的位置恒为 (0, 0),沿用摆放时的位置,只更新尺寸。
        x = memory.normal->left;
        y = memory.normal->top;
    }
    gtk_window_get_size(memory.window, &width, &height);
    if (width > 0 && height > 0) memory.normal = WindowRect{x, y, x + width, y + height};
}

gboolean sample_linux_window_bounds_when_idle(gpointer) {
    g_window_bounds.sample_source = 0;
    sample_linux_window_bounds();
    return G_SOURCE_REMOVE;
}

// configure-event 与 window-state-event 谁先到取决于窗口管理器,刚最大化时尺寸
// 可能先到;合并到空闲时再按最新状态读一次。返回 FALSE,不拦 GTK 自己的处理。
gboolean schedule_linux_window_bounds_sample(GtkWidget*, GdkEvent*, gpointer) {
    if (!g_window_bounds.sample_source) {
        g_window_bounds.sample_source =
            g_idle_add(sample_linux_window_bounds_when_idle, nullptr);
    }
    return FALSE;
}

void forget_linux_window(GtkWidget*, gpointer) {
    if (g_window_bounds.sample_source) g_source_remove(g_window_bounds.sample_source);
    g_window_bounds.sample_source = 0;
    g_window_bounds.window = nullptr;
}

std::vector<DesktopWindowMonitor> linux_monitor_work_areas(GdkDisplay* display) {
    std::vector<DesktopWindowMonitor> monitors;
    const int count = display ? gdk_display_get_n_monitors(display) : 0;
    for (int i = 0; i < count; ++i) {
        GdkMonitor* monitor = gdk_display_get_monitor(display, i);
        if (!monitor) continue;
        GdkRectangle area{};
        gdk_monitor_get_workarea(monitor, &area);
        monitors.push_back({{area.x, area.y, area.x + area.width, area.y + area.height},
                            kLogicalDesktopWindowDpi});
    }
    return monitors;
}

void move_linux_window(GtkWindow* window, WindowRect rect, bool center_hint) {
    gtk_window_resize(window, rect.right - rect.left, rect.bottom - rect.top);
    // Wayland's compositor owns placement; GTK's position hint remains useful
    // there, while X11/XWayland also supports exact work-area coordinates.
    if (center_hint) gtk_window_set_position(window, GTK_WIN_POS_CENTER);
    gtk_window_move(window, rect.left, rect.top);
}
} // namespace

void configure_linux_native_services(void* webview_widget) {
    set_linux_folder_picker(pick_linux_folder);
    if (!webview_widget) return;
    // The widget owns this noncapturing callback. Only the page native code
    // navigated to may record audio, as on Windows and macOS; camera capture,
    // other local ports and external pages remain denied.
    g_signal_connect(webview_widget, "permission-request", G_CALLBACK(
        +[](WebKitWebView* view, WebKitPermissionRequest* request, gpointer) -> gboolean {
            LinuxMediaPermissionRequest kind;
            if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
                auto* media = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request);
                kind.user_media = true;
                kind.audio = webkit_user_media_permission_is_for_audio_device(media);
                kind.video = webkit_user_media_permission_is_for_video_device(media);
            }
            const auto* trusted = static_cast<const char*>(g_object_get_data(G_OBJECT(view), kTrustedOriginKey));
            const char* uri = webkit_web_view_get_uri(view);
            switch (decide_linux_media_permission(trusted ? trusted : "", uri ? uri : "", kind)) {
            case LinuxMediaPermissionDecision::Unhandled: return FALSE;
            case LinuxMediaPermissionDecision::Allow: webkit_permission_request_allow(request); return TRUE;
            case LinuxMediaPermissionDecision::Deny: webkit_permission_request_deny(request); return TRUE;
            }
            return FALSE;
        }), nullptr);
}

void set_linux_trusted_page_origin(void* webview_widget, const std::string& origin) {
    if (!webview_widget) return;
    g_object_set_data_full(G_OBJECT(webview_widget), kTrustedOriginKey, g_strdup(origin.c_str()), g_free);
}

WindowRect linux_active_work_area() {
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return {};
    GdkMonitor* monitor = nullptr;
    GdkSeat* seat = gdk_display_get_default_seat(display);
    GdkDevice* pointer = seat ? gdk_seat_get_pointer(seat) : nullptr;
    if (pointer) {
        int x = 0;
        int y = 0;
        gdk_device_get_position(pointer, nullptr, &x, &y);
        monitor = gdk_display_get_monitor_at_point(display, x, y);
    }
    if (!monitor) monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    if (!monitor) return {};
    GdkRectangle area{};
    gdk_monitor_get_workarea(monitor, &area);
    return {area.x, area.y, area.x + area.width, area.y + area.height};
}

WindowRect center_linux_window(void* native_window, WindowRect work_area) {
    auto* window = GTK_WINDOW(native_window);
    int width = 1;
    int height = 1;
    gtk_window_get_size(window, &width, &height);
    const auto rect = fit_centered_desktop_window_rect_to_safe_work_area(
        {width, height}, work_area, 96);
    move_linux_window(window, rect, /*center_hint=*/true);
    return rect;
}

void place_linux_window_on_first_show(void* native_window,
                                      const std::optional<SavedDesktopWindowBounds>& saved) {
    auto* window = GTK_WINDOW(native_window);
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window));
    const bool positionable = linux_display_supports_window_position(display);
    WindowRect placed;
    if (saved) {
        const auto placement = place_saved_logical_desktop_window(
            *saved,
            positionable ? linux_monitor_work_areas(display)
                         : std::vector<DesktopWindowMonitor>{},
            linux_active_work_area());
        move_linux_window(window, placement.rect, /*center_hint=*/!placement.restored);
        placed = placement.rect;
        // 先摆好普通状态再最大化,还原时回到这里。
        if (saved->maximized) gtk_window_maximize(window);
    } else {
        placed = center_linux_window(window, linux_active_work_area());
    }

    // 最大化期间采样不更新普通状态,先用摆放结果填上,否则一直最大化到退出时什么也记不下。
    auto& memory = g_window_bounds;
    memory.normal = placed;
    memory.maximized = saved && saved->maximized;
    memory.positionable = positionable;
    memory.requested = positionable ? std::optional<WindowRect>(placed) : std::nullopt;
    memory.calibration_samples = 0;
    if (memory.window == window) return;
    memory.window = window;
    g_signal_connect(window, "configure-event",
                     G_CALLBACK(schedule_linux_window_bounds_sample), nullptr);
    g_signal_connect(window, "window-state-event",
                     G_CALLBACK(schedule_linux_window_bounds_sample), nullptr);
    g_signal_connect(window, "destroy", G_CALLBACK(forget_linux_window), nullptr);
}

std::optional<SavedDesktopWindowBounds> linux_window_bounds() {
    if (!g_window_bounds.normal) return std::nullopt;
    SavedDesktopWindowBounds bounds;
    bounds.rect = *g_window_bounds.normal;
    bounds.dpi = kLogicalDesktopWindowDpi;
    bounds.maximized = g_window_bounds.maximized;
    return bounds;
}

FolderPickOutcome pick_linux_folder(void* parent) {
    const std::string title(native_string(DesktopStringId::FolderPickerTitle));
    const std::string prompt(native_string(DesktopStringId::FolderPickerPrompt));
    GtkFileChooserNative* chooser = gtk_file_chooser_native_new(
        title.c_str(), GTK_WINDOW(parent), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        prompt.c_str(), nullptr);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(chooser), TRUE);
    gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(chooser), TRUE);
    FolderPickOutcome outcome;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        if (char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser))) {
            outcome.path = path;
            g_free(path);
        }
    }
    gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(chooser));
    g_object_unref(chooser);
    return outcome;
}

} // namespace acecode::desktop
