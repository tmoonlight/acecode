#include "deepin_window_effects.hpp"

#include "linux_webview_scale_policy.hpp"
#include "utils/logger.hpp"

#include <DApplication>
#include <DPlatformHandle>
#include <QWindow>

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include <cstdlib>

namespace acecode::desktop {

namespace {

bool x11_threads_ready = false;

bool is_deepin_session() {
    const char* current = std::getenv("XDG_CURRENT_DESKTOP");
    const char* session = std::getenv("XDG_SESSION_DESKTOP");
    return is_deepin_desktop(current ? current : "") ||
           is_deepin_desktop(session ? session : "");
}

} // namespace

void initialize_deepin_windowing() {
#ifdef GDK_WINDOWING_X11
    if (is_deepin_session()) {
        x11_threads_ready = XInitThreads() != 0;
    }
#endif
}

struct DeepinWindowEffects::Impl {
    // QApplication requires argc/argv to outlive it. Destroy the DTK handle
    // and foreign wrapper first; the native X11 window always belongs to GTK.
    int argc = 1;
    char name[8] = "acecode";
    char* argv[2] = {name, nullptr};
    std::unique_ptr<Dtk::Widget::DApplication> application;
    std::unique_ptr<QWindow> window;
    std::unique_ptr<Dtk::Gui::DPlatformHandle> handle;

    explicit Impl(void* native_window) {
#ifdef GDK_WINDOWING_X11
        if (!is_deepin_session()) return;
        if (!native_window) return;
        auto* widget = GTK_WIDGET(native_window);
        if (!GDK_IS_X11_DISPLAY(gtk_widget_get_display(widget))) return;
        if (!x11_threads_ready) {
            // Never let Qt initialize Xlib threading after GTK has used it.
            LOG_WARN("[desktop] early X11 thread initialization unavailable; keeping GTK window");
            return;
        }
        gtk_widget_realize(widget);
        GdkWindow* gdk_window = gtk_widget_get_window(widget);
        if (!gdk_window) return;

        if (!QCoreApplication::instance()) {
            if (!Dtk::Widget::DApplication::loadDXcbPlugin()) {
                LOG_WARN("[desktop] Deepin dxcb plugin unavailable; keeping GTK window");
                return;
            }
            application = std::make_unique<Dtk::Widget::DApplication>(argc, argv);
            application->setQuitOnLastWindowClosed(false);
        }
        if (!Dtk::Gui::DPlatformHandle::isDXcbPlatform()) return;

        window.reset(QWindow::fromWinId(gdk_x11_window_get_xid(gdk_window)));
        if (!window) return;
        handle = std::make_unique<Dtk::Gui::DPlatformHandle>(window.get());
        // Leave radius, shadow and their theme/compositor updates to DTK.
        // Do not change the native move/resize policy: GTK and the existing
        // web controls continue to initiate those operations on this window.
        const bool enabled = Dtk::Gui::DPlatformHandle::isEnabledDXcb(window.get());
        if (enabled && gtk_widget_get_mapped(widget)) {
            // WebView has already mapped this window. Deepin's compositor
            // picks up the frame clipping when it manages the window again.
            // Flush Qt's property updates before GTK remaps its native window.
            gtk_widget_hide(widget);
            QGuiApplication::sync();
            gtk_widget_show(widget);
        }
        LOG_INFO("[desktop] Deepin DTK frame effects enabled=" +
                 std::to_string(enabled) +
                 " radius=" + std::to_string(handle->windowRadius()));
#else
        (void)native_window;
#endif
    }
};

DeepinWindowEffects::DeepinWindowEffects(void* gtk_window)
    : impl_(std::make_unique<Impl>(gtk_window)) {}

DeepinWindowEffects::~DeepinWindowEffects() = default;

} // namespace acecode::desktop
