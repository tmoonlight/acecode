#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace acecode::upgrade {

std::optional<std::string> parse_executable_version_output(const std::string& output);

// Direct child process, bounded stdout and wall time, no command shell.
bool verify_executable_version(
    const std::filesystem::path& executable, const std::string& expected_version,
    std::string* error,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

} // namespace acecode::upgrade
