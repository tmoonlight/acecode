#include "linux_webview_scale_policy.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::desktop {

LinuxWebviewScalePlan plan_linux_webview_scale(
    int gtk_font_dpi, int gtk_window_scale) {
    constexpr double kBaseFontDpi = 96.0 * 1024.0;
    constexpr double kMaximumScale = 4.0;
    const double display_scale = gtk_font_dpi / kBaseFontDpi;
    if (display_scale <= 1.0 ||
        display_scale > kMaximumScale || gtk_window_scale < 1 ||
        gtk_window_scale > kMaximumScale ||
        display_scale < gtk_window_scale) {
        return {};
    }

    return {true, display_scale / gtk_window_scale,
            static_cast<int>(kBaseFontDpi)};
}

bool is_deepin_desktop(const std::string& desktop_names) {
    std::size_t start = 0;
    while (start < desktop_names.size()) {
        const auto end = desktop_names.find(':', start);
        std::string name = desktop_names.substr(start, end - start);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name == "deepin") return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

} // namespace acecode::desktop
