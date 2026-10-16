#include "helper_sandbox.hpp"

#include <cstdlib>
#include <filesystem>
#include <utility>

#ifdef __APPLE__
#include <pwd.h>
#include <unistd.h>
#endif

namespace acecode::ios_simulator {
namespace {

constexpr const char* kSandboxExec = "/usr/bin/sandbox-exec";

struct Param {
    std::string name;
    std::string value;
};

// Parameters in a fixed order; empty values are dropped together with the
// rules that would use them.
std::vector<Param> sandbox_params(const HelperSandboxPaths& paths) {
    std::vector<Param> params{{"HELPER_DIR", paths.helper_dir}, {"XCODE_APP", paths.xcode_app},
                              {"TEMP_DIR", paths.temp_dir},     {"CACHE_DIR", paths.cache_dir},
                              {"HOME_DIR", paths.home}};
    if (!paths.home.empty()) {
        const auto& home = paths.home;
        params.push_back({"SIM_DATA", home + "/Library/Developer/CoreSimulator"});
        params.push_back({"SIM_LOGS", home + "/Library/Logs/CoreSimulator"});
        params.push_back({"CORE_DEVICE", home + "/Library/Developer/CoreDevice"});
        params.push_back({"PREF_SIMULATOR", home + "/Library/Preferences/com.apple.iphonesimulator.plist"});
        params.push_back({"PREF_CORESIMULATOR", home + "/Library/Preferences/com.apple.CoreSimulator.plist"});
        params.push_back({"PREF_GLOBAL", home + "/Library/Preferences/.GlobalPreferences.plist"});
    }
    for (std::size_t index = 0; index < paths.listable_dirs.size(); ++index)
        params.push_back({"LISTABLE_" + std::to_string(index), paths.listable_dirs[index]});
    std::vector<Param> present;
    for (auto& param : params)
        if (!param.value.empty()) present.push_back(std::move(param));
    return present;
}

bool has_param(const std::vector<Param>& params, const std::string& name) {
    for (const auto& param : params)
        if (param.name == name) return true;
    return false;
}

std::string filters(const std::vector<Param>& params, const std::vector<std::pair<std::string, bool>>& wanted) {
    std::string out;
    for (const auto& [name, subtree] : wanted) {
        if (!has_param(params, name)) continue;
        out += subtree ? " (subpath (param \"" : " (literal (param \"";
        out += name + "\"))";
    }
    return out;
}

std::string listable_filters(const std::vector<Param>& params) {
    std::string out;
    for (const auto& param : params)
        if (param.name.rfind("LISTABLE_", 0) == 0) out += " (literal (param \"" + param.name + "\"))";
    return out;
}

std::string real_path(const std::string& path) {
    if (path.empty()) return {};
    std::error_code error;
    auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error || resolved.empty()) return path;
    auto text = resolved.string();
    while (text.size() > 1 && text.back() == '/') text.pop_back();
    return text;
}

#ifdef __APPLE__
std::string darwin_dir(int name) {
    char buffer[1024];
    const auto length = confstr(name, buffer, sizeof(buffer));
    if (length == 0 || length > sizeof(buffer)) return {};
    return real_path(buffer);
}
#endif

} // namespace

std::string helper_sandbox_profile(const HelperSandboxPaths& paths) {
    const auto params = sandbox_params(paths);
    std::string profile =
        "(version 1)\n"
        "(allow default)\n"
        "(deny network-outbound (remote ip \"*:*\"))\n"
        "(deny network-inbound (local ip \"*:*\"))\n"
        "(deny network-bind (local ip \"*:*\"))\n";
    profile += "(deny file-read-data (subpath \"/Users\") (subpath \"/Volumes\")" +
               filters(params, {{"HOME_DIR", true}}) + ")\n";
    const auto readable = filters(params, {{"HELPER_DIR", true}, {"XCODE_APP", true}, {"TEMP_DIR", true},
                                           {"CACHE_DIR", true}, {"SIM_DATA", true}, {"SIM_LOGS", true},
                                           {"CORE_DEVICE", true}, {"PREF_SIMULATOR", false},
                                           {"PREF_CORESIMULATOR", false}, {"PREF_GLOBAL", false}}) +
                          listable_filters(params);
    if (!readable.empty()) profile += "(allow file-read-data" + readable + ")\n";
    profile += "(deny file-write* (subpath \"/\"))\n";
    profile += "(allow file-write* (subpath \"/dev\")" +
               filters(params, {{"TEMP_DIR", true}, {"CACHE_DIR", true}, {"SIM_DATA", true},
                                {"SIM_LOGS", true}, {"CORE_DEVICE", true}}) +
               ")\n";
    return profile;
}

std::vector<std::string> sandboxed_helper_argv(const HelperSandboxPaths& paths,
                                               const std::vector<std::string>& helper_argv) {
    std::vector<std::string> argv{kSandboxExec, "-p", helper_sandbox_profile(paths)};
    for (const auto& param : sandbox_params(paths)) argv.push_back("-D" + param.name + "=" + param.value);
    argv.insert(argv.end(), helper_argv.begin(), helper_argv.end());
    return argv;
}

std::string xcode_app_for_developer_dir(const std::string& developer_dir) {
    std::string path = developer_dir;
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    const std::string suffix = "/Contents/Developer";
    if (path.size() > suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0)
        return path.substr(0, path.size() - suffix.size());
    return path;
}

HelperSandboxPaths current_helper_sandbox_paths(const std::string& helper_path,
                                                const std::string& developer_dir) {
    HelperSandboxPaths paths;
    const std::filesystem::path executable_dir = real_path(std::filesystem::path(helper_path).parent_path().string());
    paths.helper_dir = executable_dir.string();
    if (executable_dir.filename() == "MacOS" && executable_dir.parent_path().filename() == "Contents" &&
        executable_dir.parent_path().parent_path().extension() == ".app")
        paths.helper_dir = executable_dir.parent_path().parent_path().string();
    auto ancestor = executable_dir;
    for (int level = 0; level < 2 && ancestor.has_parent_path() && ancestor.parent_path() != ancestor; ++level) {
        ancestor = ancestor.parent_path();
        paths.listable_dirs.push_back(ancestor.string());
    }
    paths.xcode_app = real_path(xcode_app_for_developer_dir(developer_dir));
#ifdef __APPLE__
    // CoreSimulator finds its device set from the user record, not $HOME.
    if (const passwd* user = getpwuid(getuid()); user && user->pw_dir && *user->pw_dir)
        paths.home = real_path(user->pw_dir);
#endif
    if (paths.home.empty())
        if (const char* home = std::getenv("HOME"); home && *home) paths.home = real_path(home);
#ifdef __APPLE__
    paths.temp_dir = darwin_dir(_CS_DARWIN_USER_TEMP_DIR);
    paths.cache_dir = darwin_dir(_CS_DARWIN_USER_CACHE_DIR);
#endif
    return paths;
}

bool helper_sandbox_enabled(std::string* reason) {
    if (const char* value = std::getenv("ACECODE_IOS_SIM_SANDBOX"); value && std::string(value) == "0") {
        if (reason) *reason = "disabled by ACECODE_IOS_SIM_SANDBOX=0";
        return false;
    }
#ifdef __APPLE__
    if (access(kSandboxExec, X_OK) != 0) {
        if (reason) *reason = "/usr/bin/sandbox-exec is not available";
        return false;
    }
    return true;
#else
    if (reason) *reason = "Seatbelt is only available on macOS";
    return false;
#endif
}

} // namespace acecode::ios_simulator
