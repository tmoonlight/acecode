#include "theme_package.hpp"
#include "theme_store.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <memory>
#include <zip.h>

namespace acecode::themes {

void write_theme_archive(const std::filesystem::path& path,
                         const ThemePackageFiles& files,
                         const std::function<void(double)>& progress,
                         const std::function<bool()>& cancelled) {
    const auto is_cancelled = [&] { return cancelled && cancelled(); };
    if (is_cancelled()) throw ThemeError(499, "THEME_CANCELLED", "Theme export cancelled");
    int error = 0;
    std::unique_ptr<zip_t, decltype(&zip_discard)> archive(
        zip_open(path_to_utf8(path).c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error), zip_discard);
    if (!archive) throw ThemeError(500, "THEME_EXPORT_FAILED", "Could not create theme package", path_to_utf8(path));
    struct Callbacks {
        const std::function<void(double)>& progress;
        const std::function<bool()>& cancelled;
        bool failed = false;
    } callbacks{progress, cancelled};
    if (zip_register_progress_callback_with_state(archive.get(), 0.005,
            [](zip_t*, double value, void* context) {
                auto& callbacks = *static_cast<Callbacks*>(context);
                try { if (callbacks.progress) callbacks.progress(std::clamp(value, 0.0, 1.0)); }
                catch (...) { callbacks.failed = true; }
            }, nullptr, &callbacks) != 0 ||
        zip_register_cancel_callback_with_state(archive.get(),
            [](zip_t*, void* context) -> int {
                auto& callbacks = *static_cast<Callbacks*>(context);
                try { return callbacks.failed || (callbacks.cancelled && callbacks.cancelled()); }
                catch (...) { return 1; }
            }, nullptr, &callbacks) != 0) {
        throw ThemeError(500, "THEME_EXPORT_FAILED", "Could not observe theme compression");
    }
    for (const auto& [name, bytes] : files) {
        auto* source = zip_source_buffer(archive.get(), bytes.data(), bytes.size(), 0);
        if (!source) throw ThemeError(500, "THEME_EXPORT_FAILED", "Could not create theme package entry");
        if (zip_file_add(archive.get(), name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
            zip_source_free(source);
            throw ThemeError(500, "THEME_EXPORT_FAILED", "Could not save theme package entry");
        }
    }
    if (zip_close(archive.get()) != 0) {
        if (is_cancelled()) throw ThemeError(499, "THEME_CANCELLED", "Theme export cancelled");
        throw ThemeError(500, "THEME_EXPORT_FAILED", "Could not finish theme package", path_to_utf8(path));
    }
    archive.release();
    if (is_cancelled()) throw ThemeError(499, "THEME_CANCELLED", "Theme export cancelled");
}

} // namespace acecode::themes
