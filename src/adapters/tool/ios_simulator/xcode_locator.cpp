#include "xcode_locator.hpp"

#include "platform/process/process_runner.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef __APPLE__
#include <limits.h>
#include <unistd.h>
#endif

namespace acecode::ios_simulator {
namespace {

namespace fs = std::filesystem;

constexpr const char* kCoreSimulatorInfo =
    "/Library/Developer/PrivateFrameworks/CoreSimulator.framework/Versions/A/Resources/Info.plist";

std::string trim_trailing_slashes(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path_from_utf8(path), std::ios::binary);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool exists(const std::string& path) {
    std::error_code error;
    return fs::exists(path_from_utf8(path), error);
}

std::string canonical(const std::string& path) {
    std::error_code error;
    auto value = fs::weakly_canonical(path_from_utf8(path), error);
    return error ? path : trim_trailing_slashes(path_to_utf8(value));
}

// String value from an XML or binary property list file. Binary plists
// (e.g. the system CoreSimulator Info.plist) are read through plutil.
std::string plist_file_value(const std::string& path, const std::string& key) {
    const auto contents = read_file(path);
    if (contents.empty()) return {};
    if (contents.rfind("bplist", 0) != 0) return plist_string_value(contents, key);
    HookProcessOptions options;
    options.timeout_ms = 5000;
    auto result = run_hook_process({"/usr/bin/plutil", {"-extract", key, "raw", "-o", "-", path}}, "", "", options);
    if (result.exit_code != 0) return {};
    auto value = result.stdout_text;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    return value;
}

void add_candidate(std::vector<XcodeInstall>& installs, const std::string& path, const std::string& source) {
    if (path.empty()) return;
    auto install = inspect_xcode(path);
    if (!install) return;
    install->sources.push_back(source);
    installs.push_back(std::move(*install));
}

std::string xcode_select_target() {
#ifdef __APPLE__
    char buffer[PATH_MAX] = {};
    const auto length = ::readlink("/var/db/xcode_select_link", buffer, sizeof(buffer) - 1);
    if (length > 0) return trim_trailing_slashes(std::string(buffer, static_cast<std::size_t>(length)));
    HookProcessOptions options;
    options.timeout_ms = 3000;
    auto result = run_hook_process({"/usr/bin/xcode-select", {"-p"}}, "", "", options);
    if (result.exit_code == 0) {
        auto text = result.stdout_text;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        return trim_trailing_slashes(text);
    }
#endif
    return {};
}

void scan_applications(std::vector<XcodeInstall>& installs, const std::string& directory) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(path_from_utf8(directory), error)) {
        const auto name = path_to_utf8(entry.path().filename());
        if (name.rfind("Xcode", 0) == 0 && ends_with(name, ".app"))
            add_candidate(installs, path_to_utf8(entry.path()), "applications");
    }
}

void scan_spotlight(std::vector<XcodeInstall>& installs, int timeout_ms) {
#ifdef __APPLE__
    HookProcessOptions options;
    options.timeout_ms = timeout_ms;
    options.max_stdout_bytes = 256 * 1024;
    auto result = run_hook_process(
        {"/usr/bin/mdfind", {"kMDItemCFBundleIdentifier == 'com.apple.dt.Xcode'"}}, "", "", options);
    if (result.exit_code != 0) return;
    std::istringstream lines(result.stdout_text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Skip copies inside Trash and archives that are being expanded.
        if (line.find("/.Trash/") != std::string::npos) continue;
        add_candidate(installs, line, "spotlight");
    }
#else
    (void)installs;
    (void)timeout_ms;
#endif
}

int source_rank(const XcodeInstall& install) {
    const auto has = [&](const char* source) {
        return std::find(install.sources.begin(), install.sources.end(), source) != install.sources.end();
    };
    if (has("configured")) return 0;
    if (has("DEVELOPER_DIR")) return 1;
    if (has("xcode-select")) return 2;
    return 3;
}

} // namespace

std::string plist_string_value(const std::string& xml, const std::string& key) {
    const auto tag = "<key>" + key + "</key>";
    auto position = xml.find(tag);
    if (position == std::string::npos) return {};
    position = xml.find("<string>", position + tag.size());
    if (position == std::string::npos) return {};
    // Only accept the value directly following the key (whitespace between).
    const auto gap = xml.substr(xml.find(tag) + tag.size(), position - xml.find(tag) - tag.size());
    if (gap.find_first_not_of(" \t\r\n") != std::string::npos) return {};
    position += 8;
    const auto end = xml.find("</string>", position);
    if (end == std::string::npos) return {};
    return xml.substr(position, end - position);
}

int compare_versions(const std::string& a, const std::string& b) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() || j < b.size()) {
        long left = 0;
        long right = 0;
        while (i < a.size() && a[i] != '.') {
            if (a[i] >= '0' && a[i] <= '9') left = left * 10 + (a[i] - '0');
            ++i;
        }
        while (j < b.size() && b[j] != '.') {
            if (b[j] >= '0' && b[j] <= '9') right = right * 10 + (b[j] - '0');
            ++j;
        }
        if (left != right) return left < right ? -1 : 1;
        if (i < a.size()) ++i;
        if (j < b.size()) ++j;
    }
    return 0;
}

std::pair<std::string, std::string> xcode_paths_for(const std::string& raw) {
    const auto path = trim_trailing_slashes(raw);
    if (path.empty()) return {};
    if (ends_with(path, ".app")) return {path, path + "/Contents/Developer"};
    if (ends_with(path, ".app/Contents/Developer"))
        return {path.substr(0, path.size() - std::string("/Contents/Developer").size()), path};
    if (ends_with(path, ".app/Contents"))
        return {path.substr(0, path.size() - std::string("/Contents").size()), path + "/Developer"};
    return {};
}

std::optional<XcodeInstall> inspect_xcode(const std::string& raw, std::string* why) {
    const auto fail = [&](const std::string& reason) -> std::optional<XcodeInstall> {
        if (why) *why = reason;
        return std::nullopt;
    };
    auto [app, developer] = xcode_paths_for(raw);
    if (app.empty()) return fail("Not an Xcode.app path (expected …/Xcode.app or …/Xcode.app/Contents/Developer).");
    if (!exists(app)) return fail("The path does not exist.");
    if (!exists(developer + "/usr/bin/simctl"))
        return fail("Contents/Developer/usr/bin/simctl is missing; this is not a complete Xcode.");
    XcodeInstall install;
    install.app_path = app;
    install.developer_dir = developer;
    install.version = plist_file_value(app + "/Contents/version.plist", "CFBundleShortVersionString");
    install.build = plist_file_value(app + "/Contents/version.plist", "ProductBuildVersion");
    if (install.version.empty())
        install.version = plist_file_value(app + "/Contents/Info.plist", "CFBundleShortVersionString");
    install.simulator_kit = exists(app + "/Contents/SharedFrameworks/SimulatorKit.framework") ||
                            exists(developer + "/Library/PrivateFrameworks/SimulatorKit.framework");
    return install;
}

XcodeResolution choose_xcode(const std::string& configured_path, const std::string& configured_error,
                             std::vector<XcodeInstall> installs) {
    XcodeResolution resolution;
    resolution.configured_path = configured_path;
    resolution.configured_error = configured_error;
    // Merge duplicates found through several sources.
    std::vector<XcodeInstall> merged;
    for (auto& install : installs) {
        const auto key = canonical(install.app_path);
        auto found = std::find_if(merged.begin(), merged.end(),
            [&](const XcodeInstall& other) { return canonical(other.app_path) == key; });
        if (found == merged.end()) {
            merged.push_back(std::move(install));
            continue;
        }
        for (const auto& source : install.sources) {
            if (std::find(found->sources.begin(), found->sources.end(), source) == found->sources.end())
                found->sources.push_back(source);
        }
    }
    std::stable_sort(merged.begin(), merged.end(), [](const XcodeInstall& a, const XcodeInstall& b) {
        const int ra = source_rank(a);
        const int rb = source_rank(b);
        if (ra != rb) return ra < rb;
        return compare_versions(a.version, b.version) > 0;
    });
    resolution.installs = merged;
    if (!merged.empty()) {
        resolution.selected = merged.front();
        static const char* names[] = {"configured", "DEVELOPER_DIR", "xcode-select", "newest"};
        resolution.selected_source = names[source_rank(merged.front())];
    } else {
        resolution.error = "No Xcode installation was found. Install Xcode or set its location in "
                           "Settings > Tools > iOS Simulator.";
    }
    return resolution;
}

XcodeResolution resolve_xcode(const std::string& configured_path, const DiscoveryOptions& options) {
    std::vector<XcodeInstall> installs;
    std::string configured_error;
    if (!configured_path.empty()) {
        std::string why;
        if (auto install = inspect_xcode(configured_path, &why)) {
            install->sources.push_back("configured");
            installs.push_back(std::move(*install));
        } else {
            configured_error = why;
        }
    }
    if (const char* env = std::getenv("DEVELOPER_DIR"); env && *env) add_candidate(installs, env, "DEVELOPER_DIR");
    const auto selected = xcode_select_target();
    add_candidate(installs, selected, "xcode-select");
    scan_applications(installs, "/Applications");
    if (const char* home = std::getenv("HOME"); home && *home)
        scan_applications(installs, std::string(home) + "/Applications");
    if (options.spotlight) scan_spotlight(installs, options.spotlight_timeout_ms);
    auto resolution = choose_xcode(configured_path, configured_error, std::move(installs));
    resolution.xcode_select_path = selected;
    resolution.core_simulator_version = core_simulator_version();
    return resolution;
}

std::string core_simulator_version() {
    return plist_file_value(kCoreSimulatorInfo, "CFBundleVersion");
}

nlohmann::json to_json(const XcodeInstall& install) {
    return {{"app_path", install.app_path}, {"developer_dir", install.developer_dir},
            {"version", install.version}, {"build", install.build},
            {"simulator_kit", install.simulator_kit}, {"sources", install.sources}};
}

nlohmann::json to_json(const XcodeResolution& resolution) {
    nlohmann::json installs = nlohmann::json::array();
    for (const auto& install : resolution.installs) installs.push_back(to_json(install));
    nlohmann::json out = {{"installs", installs},
                          {"configured_path", resolution.configured_path},
                          {"configured_error", resolution.configured_error},
                          {"error", resolution.error},
                          {"xcode_select_path", resolution.xcode_select_path},
                          {"core_simulator_version", resolution.core_simulator_version},
                          {"selected_source", resolution.selected_source}};
    out["selected"] = resolution.selected ? to_json(*resolution.selected) : nlohmann::json(nullptr);
    return out;
}

} // namespace acecode::ios_simulator
