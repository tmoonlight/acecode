#include "sandbox_denial.hpp"

#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <filesystem>

namespace acecode::sandbox {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

struct Needle {
    const char* reason;
    const char* text;
};

// 顺序即优先级:先命中的原因胜出。裸 "sandbox" 刻意不收:本仓库自己就有
// src/sandbox/,一次编译错误的输出里全是 sandbox_*.cpp,非零退出就会被当成
// 沙盒拒绝、误导模型去申请越权。只认后端自己报错时的拼写。
const Needle kNeedles[] = {
    {"operation_not_permitted", "operation not permitted"},
    {"permission_denied", "permission denied"},
    {"read_only_file_system", "read-only file system"},
    {"access_denied", "access is denied"},
    {"access_denied", "access denied"},
    {"access_denied", "拒绝访问"},
    {"policy_denied", "seccomp"},
    {"policy_denied", "sandbox-exec"},
    {"policy_denied", "sandbox: deny"},
    {"policy_denied", "seatbelt"},
    {"policy_denied", "bwrap:"},
    {"policy_denied", "landlock"},
    {"failed_to_write_file", "failed to write file"},
    {"permission_denied", "eacces"},
    {"operation_not_permitted", "eperm"},
};

std::string trim_quotes(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    while (!s.empty() && (s.front() == '"' || s.front() == '\'' || s.front() == '`')) s.erase(0, 1);
    while (!s.empty() && (s.back() == '"' || s.back() == '\'' || s.back() == '`' || s.back() == ':')) s.pop_back();
    return s;
}

bool looks_like_path(const std::string& candidate) {
    if (candidate.empty()) return false;
    if (candidate[0] == '/' || candidate.compare(0, 2, "./") == 0 || candidate.compare(0, 3, "../") == 0) return true;
    if (candidate.size() >= 3 && std::isalpha(static_cast<unsigned char>(candidate[0])) && candidate[1] == ':' &&
        (candidate[2] == '\\' || candidate[2] == '/')) return true;
    if (candidate.compare(0, 2, "\\\\") == 0) return true;
    return false;
}

// 找 line 里最后一个像路径的 token(按空白切),用于 "Access is denied." 这类
// 路径在前面的 cmd / PowerShell 报错。
std::string last_path_token(const std::string& line) {
    std::string best;
    std::size_t start = 0;
    while (start <= line.size()) {
        std::size_t end = line.find_first_of(" \t", start);
        if (end == std::string::npos) end = line.size();
        std::string token = trim_quotes(line.substr(start, end - start));
        if (looks_like_path(token)) best = token;
        if (end == line.size()) break;
        start = end + 1;
    }
    return best;
}

} // namespace

bool is_likely_sandbox_denied(int exit_code, const std::string& output) {
    return classify_sandbox_violation(exit_code, output).has_value();
}

std::string extract_denied_path(const std::string& output) {
    static const char* kMarkers[] = {
        ": operation not permitted",
        ": permission denied",
        ": read-only file system",
        ": eacces",
        ": eperm",
    };
    std::size_t start = 0;
    while (start <= output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();
        const std::string line = output.substr(start, end - start);
        const std::string low = lower(line);
        // PowerShell / .NET:Access to the path 'X' is denied.
        if (const auto at = low.find("access to the path '"); at != std::string::npos) {
            const std::size_t begin = at + std::string("access to the path '").size();
            const std::size_t close = line.find('\'', begin);
            if (close != std::string::npos) {
                const std::string candidate = trim_quotes(line.substr(begin, close - begin));
                if (looks_like_path(candidate)) return candidate;
            }
        }
        for (const char* marker : kMarkers) {
            const std::size_t at = low.find(marker);
            if (at == std::string::npos) continue;
            std::string prefix = line.substr(0, at);
            // "tool: path: Permission denied" → 取最后一个 ": " 之后。
            const std::size_t sep = prefix.rfind(": ");
            if (sep != std::string::npos) prefix = prefix.substr(sep + 2);
            const std::string candidate = trim_quotes(prefix);
            if (looks_like_path(candidate)) return candidate;
            // cmd:`echo x > C:\x\y.txt` 失败时路径在前面的 token 里。
            const std::string fallback = last_path_token(line.substr(0, at));
            if (!fallback.empty()) return fallback;
        }
        if (low.find("access is denied") != std::string::npos || low.find("拒绝访问") != std::string::npos) {
            const std::string fallback = last_path_token(line);
            if (!fallback.empty()) return fallback;
        }
        if (end == output.size()) break;
        start = end + 1;
    }
    return {};
}

std::optional<SandboxViolation> classify_sandbox_violation(int exit_code, const std::string& output) {
    if (exit_code == 0) return std::nullopt;
    // 2 = 用法错误,126 = 不可执行,127 = 找不到命令:都不是沙盒的锅。
    if (exit_code == 2 || exit_code == 126 || exit_code == 127) return std::nullopt;
    const std::string text = lower(output);
    std::optional<SandboxViolation> out;
    for (const Needle& needle : kNeedles) {
        if (text.find(needle.text) == std::string::npos) continue;
        SandboxViolation v;
        v.reason = needle.reason;
        v.path = extract_denied_path(output);
        out = std::move(v);
        break;
    }
#ifndef _WIN32
    if (!out && exit_code == 128 + SIGSYS) {
        SandboxViolation v;
        v.reason = "sigsys";
        out = std::move(v);
    }
#endif
    if (!out) return std::nullopt;
    std::string snippet = output;
    while (!snippet.empty() && std::isspace(static_cast<unsigned char>(snippet.front()))) snippet.erase(0, 1);
    while (!snippet.empty() && std::isspace(static_cast<unsigned char>(snippet.back()))) snippet.pop_back();
    if (snippet.size() > 512) {
        snippet.resize(512);
        // 不在多字节 UTF-8 中间切断。
        while (!snippet.empty() && (static_cast<unsigned char>(snippet.back()) & 0xC0) == 0x80) snippet.pop_back();
        if (!snippet.empty() && (static_cast<unsigned char>(snippet.back()) & 0x80)) snippet.pop_back();
    }
    out->snippet = std::move(snippet);
    return out;
}

nlohmann::json violation_to_json(const SandboxViolation& violation) {
    nlohmann::json j = {{"reason", violation.reason}, {"snippet", violation.snippet}};
    if (!violation.path.empty()) j["path"] = violation.path;
    return j;
}

std::string suggested_write_root(const SandboxViolation& violation, const SandboxPolicy& policy) {
    if (violation.path.empty() || !is_rooted_path(violation.path)) return {};
    const auto p = path_from_utf8(violation.path);
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::is_directory(p, ec) && !ec ? p : p.parent_path();
    if (dir.empty() || dir == dir.root_path()) return {};
    const std::string candidate = normalize_policy_path(path_to_utf8(dir));
    if (candidate.empty()) return {};
    // 已经可写的目录不是「被拒」的原因;deny 名单里的目录不能通过申请变可写。
    if (policy.can_write(candidate)) return {};
    if (policy.resolve_access(candidate) == FsAccess::Deny && !policy.full_disk_read()) return {};
    for (const auto& denied : policy.denied_paths) {
        const auto rel = path_from_utf8(candidate).lexically_relative(path_from_utf8(denied));
        if (!rel.empty() && *rel.begin() != "..") return {};
    }
    for (const auto& glob : policy.denied_globs) {
        if (glob_matches_path(glob, candidate)) return {};
    }
    return candidate;
}

std::string escalation_hint(const SandboxPolicy& policy, bool network_enforced,
                            const SandboxViolation* violation, bool network_best_effort) {
    std::string hint = "\n[Sandbox] This command ran inside the ";
    hint += sandbox_mode_name(policy.mode);
    hint += " sandbox and the failure looks like a sandbox denial";
    if (violation && !violation->reason.empty()) hint += " (" + violation->reason + ")";
    if (violation && !violation->path.empty()) hint += " on path: " + violation->path;
    hint += ". ";
    if (policy.mode == SandboxMode::ReadOnly) {
        hint += "The policy has no writable roots. ";
    } else if (policy.mode == SandboxMode::WorkspaceWrite) {
        hint += "The policy permits content writes under: ";
        bool first = true;
        for (const auto& r : policy.writable_roots) {
            if (!first) hint += ", ";
            first = false;
            hint += r.root;
        }
        if (first) hint += "(none)";
        hint += " (git hooks/config and .acecode/rules are excluded from content writes). ";
    }
    if (policy.has_deny_entries()) {
        hint += "Some paths are denied entirely (secret stores such as ~/.ssh); those cannot be requested. ";
    }
    if (network_enforced) {
        hint += policy.network_access ? "Network access is allowed. " : "Network access is blocked. ";
    } else if (network_best_effort && !policy.network_access) {
        hint += "Network access is discouraged (proxies point to a dead port). ";
    }
    const std::string suggested = violation ? suggested_write_root(*violation, policy) : std::string{};
    if (!suggested.empty()) {
        hint += "If the command only needs to write under " + suggested +
                ", call bash again with sandbox_permissions=\"with_additional_permissions\", "
                "additional_permissions={\"file_system\":{\"write\":[\"" + suggested + "\"]}} and a "
                "one-sentence justification; the user will be asked to grant just that path while the "
                "command stays sandboxed. ";
    } else {
        hint += "If the command only needs a specific extra path";
        if ((network_enforced || network_best_effort) && !policy.network_access) hint += " or the network";
        hint += ", request it with sandbox_permissions=\"with_additional_permissions\" plus "
                "additional_permissions and a one-sentence justification. ";
    }
    hint += "If it genuinely needs unrestricted access, call bash again with "
            "sandbox_permissions=\"require_escalated\" and a justification; the user will be asked to "
            "approve running it outside the sandbox. Do not try to work around the sandbox by other means.";
    return hint;
}

} // namespace acecode::sandbox
