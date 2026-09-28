#include "application_icon.hpp"

#include "desktop_restart.hpp"
#include "utils/utf8_path.hpp"

#include <array>
#include <system_error>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace acecode::desktop {
namespace fs = std::filesystem;

fs::path find_application_icon_path(const fs::path& executable,
                                    const fs::path& working_directory) {
    const std::array<fs::path, 2> roots = {
        executable.parent_path(), working_directory,
    };
    for (auto root : roots) {
        for (int depth = 0; depth < 8 && !root.empty(); ++depth) {
            for (const auto* relative : {
                     "acecode-logo.png",
                     "web/public/acecode-logo.png",
                     "web/dist/acecode-logo.png",
                     "assets/windows/acecode_icon.png",
                 }) {
                const fs::path candidate = root / relative;
                std::error_code ec;
                if (fs::is_regular_file(candidate, ec) && !ec) return candidate;
            }
            const fs::path parent = root.parent_path();
            if (parent == root) break;
            root = parent;
        }
    }
    return {};
}

fs::path application_icon_path() {
    std::error_code ec;
    const fs::path working_directory = fs::current_path(ec);
    return find_application_icon_path(
        current_desktop_executable_path(), ec ? fs::path{} : working_directory);
}

#if !defined(_WIN32) && !defined(__APPLE__)
bool set_linux_window_icon(void* window, const fs::path& icon_path) {
    if (!window || icon_path.empty()) return false;
    using SetIconFromFile = int (*)(void*, const char*, void**);
    static void* const gtk = ::dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_LOCAL);
    static const auto set_icon = gtk
        ? reinterpret_cast<SetIconFromFile>(::dlsym(gtk, "gtk_window_set_icon_from_file"))
        : nullptr;
    if (!set_icon) return false;
    const std::string native_path = acecode::path_to_utf8(icon_path);
    return set_icon(window, native_path.c_str(), nullptr) != 0;
}
#endif

} // namespace acecode::desktop
