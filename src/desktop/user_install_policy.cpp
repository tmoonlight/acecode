#include "user_install_policy.hpp"

namespace acecode::desktop {

namespace {

namespace fs = std::filesystem;

fs::path normalize_absolute(const fs::path& path) {
    if (path.empty() || !path.is_absolute()) return {};
    return path.lexically_normal();
}

} // namespace

UserInstallPaths macos_user_install_paths(const fs::path& home_directory) {
    UserInstallPaths paths;
    paths.home = normalize_absolute(home_directory);
    if (paths.home.empty()) return paths;

    paths.applications = paths.home / "Applications";
    paths.destination = paths.applications / "ACECode.app";
    return paths;
}

bool macos_user_install_destination_is_safe(
    const fs::path& resolved_home,
    const fs::path& resolved_applications,
    const fs::path& resolved_destination) {
    const fs::path home = normalize_absolute(resolved_home);
    const fs::path applications = normalize_absolute(resolved_applications);
    const fs::path destination = normalize_absolute(resolved_destination);
    if (home.empty() || applications.empty() || destination.empty()) {
        return false;
    }

    const UserInstallPaths expected = macos_user_install_paths(home);
    return applications == expected.applications &&
           destination == expected.destination;
}

} // namespace acecode::desktop
