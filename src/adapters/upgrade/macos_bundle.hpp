#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace acecode::upgrade {

// Require an absolute, canonical, real ACECode.app and parent; reject App
// Translocation and bundles nested in another application. Does not check trust.
bool macos_app_install_path_is_safe(
    const std::filesystem::path& bundle,
    std::string* error = nullptr);

// Return the enclosing application bundle only for the production daemon
// layout: .../ACECode.app/Contents/MacOS/acecode-daemon. The helper is kept
// portable so layout policy can be covered by the normal unit suite.
std::optional<std::filesystem::path> macos_app_bundle_from_executable(
    const std::filesystem::path& executable);

// Locate one structurally complete ACECode.app at the staging root or inside
// one top-level package directory. The native signature verifier remains the
// authority for whether the returned bundle is trusted.
std::optional<std::filesystem::path> find_staged_macos_app_bundle(
    const std::filesystem::path& staging_dir,
    std::string* error = nullptr);

} // namespace acecode::upgrade
