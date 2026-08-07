#pragma once

#include <filesystem>
#include <string>

namespace acecode::upgrade {

// Validate a supported user or system Applications installation and
// authenticate a candidate bundle. Implemented with
// Foundation/Security.framework on macOS.
bool preflight_macos_app_update(
    const std::filesystem::path& installed_bundle,
    const std::filesystem::path& candidate_bundle,
    const std::string& expected_version,
    std::string* error = nullptr);

// Copy and re-verify the candidate beside the installed app, rotate the current
// app to .ACECode.previous.app, and switch the candidate into place. On any
// incomplete switch this function attempts to restore the previous bundle.
bool install_macos_app_update(
    const std::filesystem::path& installed_bundle,
    const std::filesystem::path& candidate_bundle,
    const std::string& expected_version,
    std::filesystem::path* backup_bundle,
    std::string* error = nullptr);

} // namespace acecode::upgrade
