#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace acecode::upgrade {

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
