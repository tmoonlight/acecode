#include "macos_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace fs = std::filesystem;

namespace acecode::upgrade {
namespace {

bool is_real_directory(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    return !ec && fs::is_directory(status);
}

bool is_real_regular_file(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    return !ec && fs::is_regular_file(status);
}

bool is_complete_bundle_layout(const fs::path& bundle) {
    return bundle.filename() == "ACECode.app" &&
           is_real_directory(bundle) &&
           is_real_directory(bundle / "Contents") &&
           is_real_directory(bundle / "Contents" / "MacOS") &&
           is_real_regular_file(bundle / "Contents" / "Info.plist") &&
           is_real_regular_file(bundle / "Contents" / "MacOS" / "ACECode") &&
           is_real_regular_file(bundle / "Contents" / "MacOS" / "acecode-daemon");
}

} // namespace

bool macos_app_install_path_is_safe(const fs::path& bundle, std::string* error) {
    auto reject = [&]() {
        if (error) *error = "macOS self-update requires an absolute real ACECode.app "
            "with a real parent, outside App Translocation and other app bundles";
        return false;
    };
    if (!bundle.is_absolute() || bundle != bundle.lexically_normal() ||
        bundle.filename() != "ACECode.app") return reject();
    for (const auto& component : bundle.parent_path()) {
        std::string name = component.string();
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name == "apptranslocation" ||
            fs::path(name).extension() == ".app") return reject();
    }
    std::error_code ec;
    if (!is_real_directory(bundle) || !is_real_directory(bundle.parent_path()) ||
        fs::canonical(bundle, ec) != bundle || ec) return reject();
    return true;
}

std::optional<fs::path> macos_app_bundle_from_executable(
    const fs::path& executable) {
    const auto generic = executable.generic_string();
    if (generic.empty() || generic.front() != '/') return std::nullopt;

    const fs::path normalized = executable.lexically_normal();
    if (normalized.filename() != "acecode-daemon") return std::nullopt;

    const fs::path macos_dir = normalized.parent_path();
    const fs::path contents_dir = macos_dir.parent_path();
    const fs::path app_dir = contents_dir.parent_path();
    if (macos_dir.filename() != "MacOS" ||
        contents_dir.filename() != "Contents" ||
        app_dir.filename() != "ACECode.app") {
        return std::nullopt;
    }
    return app_dir;
}

std::optional<fs::path> find_staged_macos_app_bundle(
    const fs::path& staging_dir,
    std::string* error) {
    std::error_code ec;
    const fs::file_status staging_status = fs::symlink_status(staging_dir, ec);
    if (ec || !fs::is_directory(staging_status)) {
        if (error) *error = "macOS update staging directory is missing or unsafe";
        return std::nullopt;
    }

    const fs::path direct = staging_dir / "ACECode.app";
    if (is_complete_bundle_layout(direct)) return direct;

    std::vector<fs::path> top_level;
    for (fs::directory_iterator it(staging_dir, ec), end; !ec && it != end; ++it) {
        top_level.push_back(it->path());
    }
    if (ec) {
        if (error) *error = "failed to inspect macOS update staging directory: " + ec.message();
        return std::nullopt;
    }
    if (top_level.size() == 1 && is_real_directory(top_level.front())) {
        const fs::path nested = top_level.front() / "ACECode.app";
        if (is_complete_bundle_layout(nested)) return nested;
    }

    if (error) {
        *error = "staged macOS update must contain one complete ACECode.app bundle";
    }
    return std::nullopt;
}

} // namespace acecode::upgrade
