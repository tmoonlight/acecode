#include "install_plan.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace acecode::installer {
namespace {

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string comparable_dir(std::string dir) {
    if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') {
        dir = dir.substr(1, dir.size() - 2);
    }
    std::replace(dir.begin(), dir.end(), '/', '\\');
    dir = trim_windows_dir(dir);
    std::transform(dir.begin(), dir.end(), dir.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return dir;
}

std::string last_component(const std::string& dir) {
    const std::string trimmed = trim_windows_dir(dir);
    const auto slash = trimmed.find_last_of("\\/");
    return slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
}

bool is_drive_root(const std::string& dir) {
    const std::string trimmed = trim_windows_dir(dir);
    return trimmed.size() <= 3 && trimmed.size() >= 2 && trimmed[1] == ':';
}

std::vector<int> version_parts(const std::string& version) {
    std::vector<int> parts;
    std::stringstream stream(version);
    std::string part;
    while (std::getline(stream, part, '.')) {
        int value = 0;
        for (char c : part) {
            if (!std::isdigit(static_cast<unsigned char>(c))) break;
            value = value * 10 + (c - '0');
            if (value > 1000000) break;
        }
        parts.push_back(value);
    }
    return parts;
}

}  // namespace

ArgsParseResult parse_installer_args(const std::vector<std::string>& argv) {
    InstallerArgs args;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        const auto value_of = [&argv, &i](const std::string& flag, const std::string& current,
                                          std::string* out) {
            const std::string prefix = flag + "=";
            if (current.rfind(prefix, 0) == 0) {
                *out = current.substr(prefix.size());
                return true;
            }
            if (current == flag && i + 1 < argv.size()) {
                *out = argv[++i];
                return true;
            }
            return false;
        };
        std::string value;
        if (arg == "--uninstall") {
            args.mode = InstallerMode::uninstall;
        } else if (arg == "--silent" || arg == "/S" || arg == "/s" || arg == "--quiet") {
            args.silent = true;
        } else if (arg == "--relaunched") {
            args.relaunched = true;
        } else if (arg.rfind("--install-dir", 0) == 0) {
            if (!value_of("--install-dir", arg, &value) || value.empty()) {
                return {std::nullopt, "--install-dir requires a directory"};
            }
            args.install_dir = trim_windows_dir(value);
        } else if (arg.rfind("--wait-pid", 0) == 0) {
            if (!value_of("--wait-pid", arg, &value) || value.empty() ||
                !std::all_of(value.begin(), value.end(),
                             [](unsigned char c) { return std::isdigit(c) != 0; }) ||
                value.size() > 10) {
                return {std::nullopt, "--wait-pid requires a process id"};
            }
            args.wait_pid = static_cast<unsigned long>(std::stoull(value));
        } else {
            return {std::nullopt, "unknown argument: " + arg};
        }
    }
    return {args, {}};
}

std::vector<std::string> uninstall_relaunch_args(const InstallerArgs& args,
                                                 unsigned long parent_pid,
                                                 const std::string& install_dir) {
    std::vector<std::string> out{"--uninstall", "--relaunched", "--wait-pid",
                                 std::to_string(parent_pid), "--install-dir", install_dir};
    if (args.silent) out.push_back("--silent");
    return out;
}

std::string quote_windows_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
        } else {
            out.append(backslashes, '\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

std::string trim_windows_dir(const std::string& dir) {
    std::string out = dir;
    while (out.size() > 3 && (out.back() == '\\' || out.back() == '/')) out.pop_back();
    if (out.size() == 3 && out[1] == ':' && out.back() == '/') out.back() = '\\';
    return out;
}

std::string join_windows_path(const std::string& dir, const std::string& name) {
    std::string base = trim_windows_dir(dir);
    if (base.empty()) return name;
    if (base.back() != '\\' && base.back() != '/') base.push_back('\\');
    return base + name;
}

std::string default_install_dir(const std::string& local_app_data) {
    return join_windows_path(join_windows_path(local_app_data, "Programs"), kProductName);
}

std::string normalize_chosen_install_dir(const std::string& picked) {
    std::string dir = picked;
    std::replace(dir.begin(), dir.end(), '/', '\\');
    dir = trim_windows_dir(dir);
    if (dir.empty()) return dir;
    if (iequals(last_component(dir), kProductName) && !is_drive_root(dir)) return dir;
    return join_windows_path(dir, kProductName);
}

bool is_acceptable_install_dir(const std::string& dir) {
    const std::string trimmed = trim_windows_dir(dir);
    if (trimmed.size() < 3) return false;
    const bool drive = std::isalpha(static_cast<unsigned char>(trimmed[0])) &&
                       trimmed[1] == ':' && (trimmed[2] == '\\' || trimmed[2] == '/');
    const bool unc = trimmed.rfind("\\\\", 0) == 0 && trimmed.size() > 4;
    if (!drive && !unc) return false;
    if (drive && is_drive_root(trimmed)) return false;
    if (trimmed.find_first_of("<>\"|?*") != std::string::npos) return false;
    return trimmed.find(':', 2) == std::string::npos;
}

std::vector<std::string> split_path_list(const std::string& value) {
    std::vector<std::string> entries;
    std::string current;
    for (char c : value) {
        if (c == ';') {
            entries.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    entries.push_back(current);
    return entries;
}

bool path_list_contains(const std::string& value, const std::string& dir) {
    const std::string wanted = comparable_dir(dir);
    if (wanted.empty()) return false;
    for (const auto& entry : split_path_list(value)) {
        if (comparable_dir(entry) == wanted) return true;
    }
    return false;
}

std::string path_list_with(const std::string& value, const std::string& dir) {
    if (path_list_contains(value, dir)) return value;
    std::string base = value;
    while (!base.empty() && base.back() == ';') base.pop_back();
    return base.empty() ? dir : base + ";" + dir;
}

std::string path_list_without(const std::string& value, const std::string& dir) {
    if (!path_list_contains(value, dir)) return value;
    const std::string wanted = comparable_dir(dir);
    std::string out;
    bool first = true;
    for (const auto& entry : split_path_list(value)) {
        if (!entry.empty() && comparable_dir(entry) == wanted) continue;
        if (!first) out.push_back(';');
        out += entry;
        first = false;
    }
    return out;
}

std::vector<RegistryValue> uninstall_entry_values(const UninstallEntryInput& input) {
    const std::string dir = trim_windows_dir(input.install_dir);
    const std::string uninstaller = quote_windows_arg(join_windows_path(dir, kUninstallerFileName));
    const std::uint64_t kilobytes = (input.installed_bytes + 1023) / 1024;
    const auto text = [](const char* name, std::string value) {
        RegistryValue item;
        item.name = name;
        item.text = std::move(value);
        return item;
    };
    const auto dword = [](const char* name, std::uint32_t value) {
        RegistryValue item;
        item.name = name;
        item.is_dword = true;
        item.number = value;
        return item;
    };
    std::vector<RegistryValue> values{
        text("DisplayName", kProductName),
        text("DisplayVersion", input.version),
        text("Publisher", kPublisher),
        text("DisplayIcon", join_windows_path(dir, kDesktopExecutable) + ",0"),
        text("InstallLocation", dir),
        text("UninstallString", uninstaller + " --uninstall"),
        text("QuietUninstallString", uninstaller + " --uninstall --silent"),
        text("URLInfoAbout", kProductUrl),
        text("HelpLink", kProductUrl),
        dword("NoModify", 1),
        dword("NoRepair", 1),
        dword("EstimatedSize", static_cast<std::uint32_t>(
            std::min<std::uint64_t>(kilobytes, 0xFFFFFFFFu))),
    };
    if (!input.install_date.empty()) values.push_back(text("InstallDate", input.install_date));
    return values;
}

std::string format_install_date(int year, int month, int day) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d", year, month, day);
    return buffer;
}

double overall_progress(InstallStep step, double step_fraction) {
    const double f = std::clamp(step_fraction, 0.0, 1.0);
    // 每段终点直接取边界值,避免 0.04 + 0.78 这类浮点和略大于下一段起点而倒退。
    const auto span = [f](double from, double to) { return f >= 1.0 ? to : from + (to - from) * f; };
    switch (step) {
        case InstallStep::preparing: return span(0.0, 0.04);
        case InstallStep::extracting: return span(0.04, 0.82);
        case InstallStep::replacing: return span(0.82, 0.88);
        case InstallStep::integrating: return span(0.88, 0.99);
        case InstallStep::finished: return 1.0;
    }
    return 0.0;
}

int compare_versions(const std::string& a, const std::string& b) {
    const auto left = version_parts(a);
    const auto right = version_parts(b);
    const std::size_t count = std::max(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int l = i < left.size() ? left[i] : 0;
        const int r = i < right.size() ? right[i] : 0;
        if (l != r) return l < r ? -1 : 1;
    }
    return 0;
}

InstallAction classify_install(const std::optional<std::string>& installed_version,
                               const std::string& payload_version) {
    if (!installed_version || installed_version->empty()) return InstallAction::install;
    const int order = compare_versions(*installed_version, payload_version);
    if (order == 0) return InstallAction::reinstall;
    return order < 0 ? InstallAction::update : InstallAction::install;
}

}  // namespace acecode::installer
