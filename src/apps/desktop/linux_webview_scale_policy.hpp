#pragma once

#include <string>

namespace acecode::desktop {

struct LinuxWebviewScalePlan {
    bool apply = false;
    double page_zoom = 1.0;
    int font_dpi = 0;
};

// Native XSettings stores font DPI in units of 1/1024 DPI. Deepin's separate
// scale-factor preference can remain 1.0 while this effective DPI is 120.
LinuxWebviewScalePlan plan_linux_webview_scale(
    int gtk_font_dpi, int gtk_window_scale);

bool is_deepin_desktop(const std::string& desktop_names);

} // namespace acecode::desktop
