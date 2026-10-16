#pragma once

// Seatbelt confinement for acecode-ios-sim. The helper reaches CoreSimulator,
// SimulatorKit and the simulator's accessibility over XPC / Mach and returns
// pixels over stdout; it never needs IP networking or the user's documents.
// The profile keeps system services reachable (allow default) and removes
// what a compromised helper could abuse:
//   - IP networking (inbound, outbound, bind); Unix sockets stay available;
//   - reading file contents under /Users, /Volumes and the home directory,
//     except the helper's bundle, the selected Xcode, simulator data/logs,
//     the simulator preference files and the per-user temp / cache folders;
//   - writing anywhere except /dev, simulator data/logs and temp / cache.
// The profile text is ACECode's own and is passed to /usr/bin/sandbox-exec
// with every path as a -D parameter, so no path is ever spliced into SBPL.

#include <string>
#include <vector>

namespace acecode::ios_simulator {

struct HelperSandboxPaths {
    // The helper's .app bundle (…/ACECode.app) or, outside a bundle, the
    // directory holding the executable.
    std::string helper_dir;
    // Directories CFBundle lists while locating the main bundle (the
    // executable's ancestors up to the would-be X.app of X.app/Contents/MacOS).
    // Listing only: their other contents stay unreadable. Without them
    // CoreSimulator's NSUserDefaults setup crashes on a NULL main bundle.
    std::vector<std::string> listable_dirs;
    std::string xcode_app;   // .../Xcode.app (or the developer dir itself)
    std::string home;        // the user's home directory (user record, as CoreSimulator uses)
    std::string temp_dir;    // DARWIN_USER_TEMP_DIR
    std::string cache_dir;   // DARWIN_USER_CACHE_DIR
};

// Profile for the given paths; rules whose path is empty are omitted.
std::string helper_sandbox_profile(const HelperSandboxPaths& paths);

// `/usr/bin/sandbox-exec -p <profile> -D… <helper_argv…>`.
std::vector<std::string> sandboxed_helper_argv(const HelperSandboxPaths& paths,
                                               const std::vector<std::string>& helper_argv);

// .../Xcode.app/Contents/Developer -> .../Xcode.app; other shapes unchanged.
std::string xcode_app_for_developer_dir(const std::string& developer_dir);

// Real (symlink-resolved) paths for this process: Seatbelt matches resolved
// vnode paths, so /var/folders must become /private/var/folders.
HelperSandboxPaths current_helper_sandbox_paths(const std::string& helper_path,
                                                const std::string& developer_dir);

// False when ACECODE_IOS_SIM_SANDBOX=0 or sandbox-exec is unavailable; the
// reason is filled in that case.
bool helper_sandbox_enabled(std::string* reason = nullptr);

} // namespace acecode::ios_simulator
