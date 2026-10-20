#pragma once

#include "platform/native_ui/folder_picker.hpp"
#include "window_size.hpp"

#include <string>

namespace acecode::desktop {

// GTK uses logical coordinates for both monitor work areas and window sizes.
WindowRect linux_active_work_area();
void center_linux_window(void* window, WindowRect work_area);
FolderPickOutcome pick_linux_folder(void* parent);
void configure_linux_native_services(void* webview_widget);
// Origin of the page native code navigated to; only it may use the microphone.
void set_linux_trusted_page_origin(void* webview_widget, const std::string& origin);

} // namespace acecode::desktop
