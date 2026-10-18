#include "linux_desktop.hpp"

#include "platform/native_ui/strings.hpp"

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

namespace acecode::desktop {

void configure_linux_native_services(void* webview_widget) {
    set_linux_folder_picker(pick_linux_folder);
    if (!webview_widget) return;
    // The widget owns this noncapturing callback. Only the local main UI can
    // request audio; camera capture and external pages remain denied.
    g_signal_connect(webview_widget, "permission-request", G_CALLBACK(
        +[](WebKitWebView* view, WebKitPermissionRequest* request, gpointer) -> gboolean {
            if (!WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) return FALSE;
            auto* media = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request);
            const char* uri = webkit_web_view_get_uri(view);
            const std::string url = uri ? uri : "";
            const bool local = url.rfind("http://127.0.0.1:", 0) == 0 ||
                url.rfind("http://localhost:", 0) == 0 || url.rfind("http://[::1]:", 0) == 0;
            if (local && webkit_user_media_permission_request_is_for_audio_device(media) &&
                !webkit_user_media_permission_request_is_for_video_device(media)) {
                webkit_permission_request_allow(request);
            } else {
                webkit_permission_request_deny(request);
            }
            return TRUE;
        }), nullptr);
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

void center_linux_window(void* native_window, WindowRect work_area) {
    auto* window = GTK_WINDOW(native_window);
    int width = 1;
    int height = 1;
    gtk_window_get_size(window, &width, &height);
    const auto rect = fit_centered_desktop_window_rect_to_safe_work_area(
        {width, height}, work_area, 96);
    gtk_window_resize(window, rect.right - rect.left, rect.bottom - rect.top);
    // Wayland's compositor owns placement; GTK's position hint remains useful
    // there, while X11/XWayland also supports exact work-area coordinates.
    gtk_window_set_position(window, GTK_WIN_POS_CENTER);
    gtk_window_move(window, rect.left, rect.top);
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
