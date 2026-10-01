#include "process_environment.hpp"
#include "ipc/runtime_files.hpp"
#include "platform/package_identity.hpp"
#include "platform/process/os_process.hpp"
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace acecode::cli {

void configure_process_environment() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // SECURITY: Prevent Windows from executing commands from current directory.
    // Without this, a malicious exe placed in cwd could hijack system commands.
    SetEnvironmentVariableA("NoDefaultCurrentDirectoryInExePath", "1");
#endif
}

std::string get_executable_dir_from_argv(int argc, char* argv[]) {
    if (has_windows_package_identity()) {
        // An AppExecutionAlias points into WindowsApps, not beside our resources.
        if (auto path = daemon::process_executable_path(daemon::current_pid())) {
            return std::filesystem::u8path(*path).parent_path().u8string();
        }
    }
    if (argc <= 0 || !argv[0]) return "";
    std::error_code ec;
    std::filesystem::path exe(argv[0]);
    std::filesystem::path abs = std::filesystem::weakly_canonical(exe, ec);
    if (!ec) return abs.parent_path().string();
    return exe.parent_path().string();
}


} // namespace acecode::cli
