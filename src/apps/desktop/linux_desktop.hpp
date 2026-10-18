#pragma once

#include "platform/native_ui/folder_picker.hpp"
#include "window_size.hpp"

namespace acecode::desktop {

// GTK uses logical coordinates for both monitor work areas and window sizes.
WindowRect linux_active_work_area();
void center_linux_window(void* window, WindowRect work_area);
FolderPickOutcome pick_linux_folder(void* parent);
void configure_linux_native_services(void* webview_widget);

} // namespace acecode::desktop
