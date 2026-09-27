#include "toolchains.hpp"

#include "platform/process/which.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace acecode::environment {
namespace {
std::string original_process_path();
std::vector<std::string> split_path_list(const std::string&, char);
}

const std::vector<std::string>& toolchain_ids() {
    static const std::vector<std::string> ids = {"python", "node", "csharp"};
    return ids;
}

std::string toolchain_label(const std::string& id) {
    if (id == "python") return "Python";
    if (id == "node") return "Node.js";
    if (id == "csharp") return "C#";
    return id;
}

std::vector<std::string> toolchain_anchor_commands(const std::string& id) {
    if (id == "python") return {"python", "python3"};
    if (id == "node") return {"node"};
    if (id == "csharp") return {"dotnet"};
    return {};
}

bool is_windows_app_execution_alias(const std::string& path) {
    std::string lower;
    lower.reserve(path.size());
    for (char ch : path) {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        lower.push_back(c == '/' ? '\\' : c);
    }
    return lower.find("\\microsoft\\windowsapps\\") != std::string::npos;
}

std::string ToolchainDetection::dir_for(const std::string& id) const {
    auto it = dirs.find(id);
    return it == dirs.end() ? std::string{} : it->second;
}

ToolchainDetection detect_toolchains(const WhichFn& which) {
    ToolchainDetection out;
    for (const auto& id : toolchain_ids()) {
        for (const auto& anchor : toolchain_anchor_commands(id)) {
            auto hit = which(anchor);
            if (!hit || hit->empty()) continue;
            if (is_windows_app_execution_alias(*hit)) continue;  // 商店桩不算安装
            const auto parent = path_from_utf8(*hit).parent_path();
            if (parent.empty()) continue;
            out.dirs[id] = path_to_utf8(parent);
            out.anchors[id] = *hit;
            break;
        }
    }
    return out;
}

ToolchainDetection detect_toolchains() {
    const auto directories = split_path_list(original_process_path(), path_list_separator());
#ifdef _WIN32
    const std::vector<std::string> extensions = {".exe", ".cmd", ".bat", ".com"};
#else
    const std::vector<std::string> extensions;
#endif
    return detect_toolchains([&](const std::string& command) {
        return lsp::which_in(command, directories, extensions, [](const std::string& file) {
            if (is_windows_app_execution_alias(file)) return false;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path_from_utf8(file), ec) || ec) return false;
#ifndef _WIN32
            return ::access(file.c_str(), X_OK) == 0;
#else
            return true;
#endif
        });
    });
}

std::string find_toolchain_anchor_in_dir(const std::string& id, const std::string& dir) {
    if (dir.empty()) return {};
#ifdef _WIN32
    const std::vector<std::string> pathext = {".exe", ".cmd", ".bat", ".com"};
#else
    const std::vector<std::string> pathext;
#endif
    const auto exists = [](const std::string& path) {
        std::error_code ec;
        return std::filesystem::is_regular_file(path_from_utf8(path), ec) && !ec;
    };
    for (const auto& anchor : toolchain_anchor_commands(id)) {
        if (auto hit = lsp::which_in(anchor, {dir}, pathext, exists)) return *hit;
    }
    return {};
}

std::string& toolchain_dir_ref(ToolchainsConfig& cfg, const std::string& id) {
    if (id == "python") return cfg.python;
    if (id == "node") return cfg.node;
    return cfg.csharp;
}

std::string toolchain_dir(const ToolchainsConfig& cfg, const std::string& id) {
    if (id == "python") return cfg.python;
    if (id == "node") return cfg.node;
    if (id == "csharp") return cfg.csharp;
    return {};
}

bool merge_detected_toolchains(ToolchainsConfig& cfg, const ToolchainDetection& detected) {
    bool changed = false;
    for (const auto& id : toolchain_ids()) {
        const std::string dir = detected.dir_for(id);
        if (dir.empty()) continue;
        std::string& slot = toolchain_dir_ref(cfg, id);
        if (slot != dir) {
            slot = dir;
            changed = true;
        }
    }
    return changed;
}

bool fill_unset_toolchains(ToolchainsConfig& cfg, const ToolchainDetection& detected) {
    bool changed = false;
    for (const auto& id : toolchain_ids()) {
        std::string& slot = toolchain_dir_ref(cfg, id);
        if (!slot.empty()) continue;
        const std::string dir = detected.dir_for(id);
        if (dir.empty()) continue;
        slot = dir;
        changed = true;
    }
    return changed;
}

std::vector<std::pair<std::string, std::string>> configured_toolchain_dirs(
    const ToolchainsConfig& cfg) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& id : toolchain_ids()) {
        const std::string dir = toolchain_dir(cfg, id);
        if (!dir.empty()) out.emplace_back(toolchain_label(id), dir);
    }
    return out;
}

namespace {

// 比较键:去尾部分隔符;Windows 再统一小写与反斜杠。
std::string path_compare_key(const std::string& entry) {
    std::string key = entry;
    while (key.size() > 1 && (key.back() == '\\' || key.back() == '/')) key.pop_back();
#ifdef _WIN32
    for (auto& ch : key) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch == '/') ch = '\\';
    }
#endif
    return key;
}

std::vector<std::string> split_path_list(const std::string& value, char separator) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : value) {
        if (c == separator) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

struct ToolchainRuntime {
    std::mutex mu;
    std::optional<std::string> original_path;
    std::vector<std::string> injected;                             // 上次注入的目录
    std::vector<std::pair<std::string, std::string>> applied;      // (label, dir)
};

ToolchainRuntime& runtime() {
    static ToolchainRuntime rt;
    return rt;
}

std::string original_process_path() {
    auto& rt = runtime();
    std::lock_guard<std::mutex> lk(rt.mu);
    return rt.original_path.value_or(current_process_path());
}

}  // namespace

std::string compute_path_with_prefix(const std::string& current_path,
                                     const std::vector<std::string>& previously_injected,
                                     const std::vector<std::string>& new_dirs,
                                     char separator) {
    std::vector<std::string> remove_keys;
    for (const auto& d : previously_injected) remove_keys.push_back(path_compare_key(d));
    for (const auto& d : new_dirs) remove_keys.push_back(path_compare_key(d));

    std::vector<std::string> kept;
    for (const auto& entry : split_path_list(current_path, separator)) {
        if (entry.empty()) continue;
        const std::string key = path_compare_key(entry);
        if (std::find(remove_keys.begin(), remove_keys.end(), key) != remove_keys.end()) continue;
        kept.push_back(entry);
    }

    std::vector<std::string> prefix;
    for (const auto& d : new_dirs) {
        if (d.empty()) continue;
        const std::string key = path_compare_key(d);
        bool dup = false;
        for (const auto& p : prefix) {
            if (path_compare_key(p) == key) { dup = true; break; }
        }
        if (!dup) prefix.push_back(d);
    }

    std::string out;
    auto append = [&](const std::string& entry) {
        if (!out.empty()) out.push_back(separator);
        out += entry;
    };
    for (const auto& p : prefix) append(p);
    for (const auto& k : kept) append(k);
    return out;
}

char path_list_separator() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

std::string current_process_path() {
    return getenv_utf8("PATH");
}

void set_process_path(const std::string& value) {
#ifdef _WIN32
    const std::wstring wide = utf8_to_wide(value);
    ::SetEnvironmentVariableW(L"PATH", wide.c_str());
    // CRT 副本也同步,免得 getenv("PATH") 的老调用方看到旧值。
    _wputenv_s(L"PATH", wide.c_str());
#else
    ::setenv("PATH", value.c_str(), 1);
#endif
}

PathPrefixResult apply_toolchain_path(const ToolchainsConfig& cfg) {
    PathPrefixResult result;
    std::vector<std::string> new_dirs;
    for (const auto& [label, dir] : configured_toolchain_dirs(cfg)) {
        std::error_code ec;
        if (std::filesystem::is_directory(path_from_utf8(dir), ec) && !ec) {
            new_dirs.push_back(dir);
            result.applied.emplace_back(label, dir);
        } else {
            result.skipped.emplace_back(label, dir);
            LOG_INFO("[toolchains] skipping missing directory: " + label + "=" + dir);
        }
    }

    auto& rt = runtime();
    std::lock_guard<std::mutex> lk(rt.mu);
    if (!rt.original_path) rt.original_path = current_process_path();
    const std::string updated = compute_path_with_prefix(
        *rt.original_path, {}, new_dirs, path_list_separator());
    set_process_path(updated);
    rt.injected = new_dirs;
    rt.applied = result.applied;
    if (!new_dirs.empty()) {
        std::string joined;
        for (const auto& d : new_dirs) {
            if (!joined.empty()) joined += ", ";
            joined += d;
        }
        LOG_INFO("[toolchains] PATH prefix applied: " + joined);
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> applied_toolchain_dirs() {
    auto& rt = runtime();
    std::lock_guard<std::mutex> lk(rt.mu);
    return rt.applied;
}

void reset_toolchain_runtime_for_test() {
    auto& rt = runtime();
    std::lock_guard<std::mutex> lk(rt.mu);
    rt.injected.clear();
    rt.applied.clear();
    rt.original_path.reset();
}

}  // namespace acecode::environment
