#include "security_handler.hpp"

#include "sandbox/sandbox_policy.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace acecode::web {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string trim_copy(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t' ||
                           value[begin] == '\r' || value[begin] == '\n')) ++begin;
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\r' || value[end - 1] == '\n')) --end;
    return value.substr(begin, end - begin);
}

bool starts_with_token(const std::string& entry, const char* token) {
    const std::string t = token;
    if (entry.compare(0, t.size(), t) != 0) return false;
    return entry.size() == t.size() || entry[t.size()] == '/' || entry[t.size()] == '\\';
}

// 读取一张清单:必须是字符串数组;去空白、去空项、去重(保序)。
bool read_entry_list(const json& value, const std::string& field, bool allow_glob,
                     std::vector<std::string>& out, std::string& error) {
    if (!value.is_array()) {
        error = field + " must be an array of strings";
        return false;
    }
    std::vector<std::string> items;
    std::set<std::string> seen;
    for (const auto& item : value) {
        if (!item.is_string()) {
            error = field + " must be an array of strings";
            return false;
        }
        const std::string entry = trim_copy(item.get<std::string>());
        if (entry.empty()) continue;
        const std::string problem = validate_sandbox_entry(entry, allow_glob);
        if (!problem.empty()) {
            error = problem;
            return false;
        }
        if (seen.insert(entry).second) items.push_back(entry);
    }
    out = std::move(items);
    return true;
}

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool parse_pattern(const json& value, std::vector<std::vector<std::string>>& out, std::string& error) {
    if (!value.is_array() || value.empty()) {
        error = "pattern must be a non-empty array";
        return false;
    }
    std::vector<std::vector<std::string>> pattern;
    for (const auto& position : value) {
        std::vector<std::string> alternatives;
        if (position.is_string()) {
            alternatives.push_back(trim_copy(position.get<std::string>()));
        } else if (position.is_array() && !position.empty()) {
            for (const auto& alternative : position) {
                if (!alternative.is_string()) {
                    error = "pattern alternatives must be strings";
                    return false;
                }
                alternatives.push_back(trim_copy(alternative.get<std::string>()));
            }
        } else {
            error = "pattern items must be strings or non-empty string arrays";
            return false;
        }
        for (const auto& token : alternatives) {
            if (token.empty()) {
                error = "pattern tokens must not be empty";
                return false;
            }
        }
        pattern.push_back(std::move(alternatives));
    }
    out = std::move(pattern);
    return true;
}

// 首 token 是这些命令时,任何更长的放行前缀都拒绝(与前端 BANNED_ALLOW_HEADS 同源)。
// 比较前去掉目录与 .exe / .cmd / .bat 后缀,大小写不敏感。
bool is_dangerous_allow_head(const std::string& token) {
    static const std::set<std::string> heads = {
        "bash", "sh", "zsh", "fish", "dash", "ksh", "csh", "tcsh", "cmd", "powershell", "pwsh",
        "python", "python3", "pythonw", "pyw", "py", "node", "nodejs", "deno", "bun", "ruby", "perl",
        "php", "lua", "julia", "osascript", "rm", "del", "erase", "rmdir", "rd", "sudo", "doas", "su",
        "runas", "eval", "exec", "source", "xargs", "env", "nohup",
    };
    std::string name = token;
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* suffix : {".exe", ".cmd", ".bat", ".com"}) {
        const std::string s = suffix;
        if (name.size() > s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0) {
            name.erase(name.size() - s.size());
            break;
        }
    }
    return heads.count(name) > 0;
}

std::string pattern_display(const std::vector<std::vector<std::string>>& pattern) {
    std::string out;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (i) out += ' ';
        for (std::size_t j = 0; j < pattern[i].size(); ++j) {
            if (j) out += '|';
            out += pattern[i][j];
        }
    }
    return out;
}

bool parse_i64(const std::string& text, std::int64_t& out) {
    if (text.empty()) return false;
    try {
        std::size_t consumed = 0;
        const long long value = std::stoll(text, &consumed);
        if (consumed != text.size()) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

const std::set<std::string>& known_categories() {
    static const std::set<std::string> values = {
        security::kAuditCategoryCommand, security::kAuditCategoryFile, security::kAuditCategoryTool,
        security::kAuditCategorySandbox, security::kAuditCategoryRule};
    return values;
}

const std::set<std::string>& known_decisions() {
    static const std::set<std::string> values = {
        security::kAuditDecisionAllow, security::kAuditDecisionAllowSession,
        security::kAuditDecisionAllowScoped, security::kAuditDecisionAllowRemember,
        security::kAuditDecisionDeny, security::kAuditDecisionForbidden, security::kAuditDecisionBlocked};
    return values;
}

} // namespace

// ---------------------------------------------------------------------------
// 沙盒配置
// ---------------------------------------------------------------------------

json sandbox_settings_snapshot(const SandboxConfig& sandbox, const sandbox::BackendProbe& probe) {
    const bool read_isolation = probe.kind == sandbox::BackendKind::MacosSeatbelt ||
                                probe.kind == sandbox::BackendKind::LinuxBwrap;
#if defined(_WIN32)
    const char* os = "windows";
#elif defined(__APPLE__)
    const char* os = "macos";
#else
    const char* os = "linux";
#endif
    return json{
        {"enabled", sandbox.enabled},
        {"network_access", sandbox.network_access},
        {"deny_defaults", sandbox.deny_defaults},
        {"filesystem", json{{"read", sandbox.filesystem_read},
                            {"write", sandbox.filesystem_write},
                            {"deny", sandbox.filesystem_deny}}},
        {"windows_backend", sandbox.windows_backend.empty() ? std::string("restricted-token")
                                                           : sandbox.windows_backend},
        {"writable_roots", sandbox.writable_roots},
        {"exclude_tmpdir", sandbox.exclude_tmpdir},
        {"defaults", json{{"deny", sandbox::default_denied_entries()}}},
        {"platform", json{{"os", os},
                          {"backend", sandbox::backend_kind_name(probe.kind)},
                          {"available", probe.available},
                          {"reason", probe.reason},
                          {"network_enforced", probe.network_enforced},
                          {"network_best_effort", probe.network_best_effort},
                          {"read_isolation", read_isolation}}},
    };
}

std::string validate_sandbox_entry(const std::string& raw, bool allow_glob) {
    const std::string entry = trim_copy(raw);
    if (entry.empty()) return "entry must not be empty";
    if (entry.find('\n') != std::string::npos || entry.find('\r') != std::string::npos) {
        return "entry must be a single line: " + entry;
    }
    const bool has_glob = entry.find_first_of("*?[") != std::string::npos;
    if (has_glob && !allow_glob) return "wildcards are only allowed in the deny list: " + entry;
    if (entry[0] == '~') {
        if (entry.size() > 1 && entry[1] != '/' && entry[1] != '\\') {
            return "only `~` or `~/path` is supported (no `~user`): " + entry;
        }
        return {};
    }
    if (entry[0] == ':') {
        if (starts_with_token(entry, ":workspace_roots") || starts_with_token(entry, ":tmpdir") ||
            starts_with_token(entry, ":acecode_home")) {
            return {};
        }
        return "unknown token (use :workspace_roots, :tmpdir or :acecode_home): " + entry;
    }
    if (has_glob && entry.compare(0, 3, "**/") == 0) return {};
    if (has_glob && entry.compare(0, 3, "**\\") == 0) return {};
    if (!sandbox::is_rooted_path(entry)) return "path must be absolute: " + entry;
    return {};
}

bool parse_sandbox_settings_request(const json& body, SandboxConfig& out,
                                    std::string& error, std::string& field) {
    if (!body.is_object()) {
        error = "body must be an object";
        field = "body";
        return false;
    }
    SandboxConfig next = out;
    for (const char* flag : {"enabled", "network_access", "deny_defaults"}) {
        if (!body.contains(flag)) continue;
        if (!body[flag].is_boolean()) {
            error = std::string(flag) + " must be a boolean";
            field = flag;
            return false;
        }
        const bool value = body[flag].get<bool>();
        if (std::string(flag) == "enabled") next.enabled = value;
        else if (std::string(flag) == "network_access") next.network_access = value;
        else next.deny_defaults = value;
    }
    if (body.contains("filesystem")) {
        const auto& filesystem = body["filesystem"];
        if (!filesystem.is_object()) {
            error = "filesystem must be an object";
            field = "filesystem";
            return false;
        }
        struct ListSpec { const char* key; std::vector<std::string>* target; bool glob; };
        const ListSpec specs[] = {
            {"read", &next.filesystem_read, false},
            {"write", &next.filesystem_write, false},
            {"deny", &next.filesystem_deny, true},
        };
        for (const auto& spec : specs) {
            if (!filesystem.contains(spec.key)) continue;
            std::vector<std::string> items;
            const std::string name = std::string("filesystem.") + spec.key;
            if (!read_entry_list(filesystem[spec.key], name, spec.glob, items, error)) {
                field = name;
                return false;
            }
            *spec.target = std::move(items);
        }
    }
    out = std::move(next);
    error.clear();
    field.clear();
    return true;
}

// ---------------------------------------------------------------------------
// 命令规则
// ---------------------------------------------------------------------------

std::vector<ExecRulesFileSnapshot> read_exec_rules_dir(const std::string& dir) {
    std::map<std::string, ExecRulesFileSnapshot> by_name;
    const fs::path root = path_from_utf8(dir);
    std::error_code ec;
    if (fs::is_directory(root, ec) && !ec) {
        for (const auto& item : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!item.is_regular_file(ec)) continue;
            const std::string name = path_to_utf8(item.path().filename());
            if (name.size() < 6 || name.compare(name.size() - 6, 6, ".rules") != 0) continue;
            ExecRulesFileSnapshot snapshot;
            snapshot.name = name;
            snapshot.path = path_to_utf8(item.path());
            snapshot.managed = sandbox::is_managed_rules_file(name);
            snapshot.scope = sandbox::rules_file_scope(name);
            snapshot.exists = true;
            const auto parsed = sandbox::parse_rules_text(read_text_file(item.path()), snapshot.scope, name);
            snapshot.error = parsed.error;
            snapshot.rules = parsed.rules;
            by_name[name] = std::move(snapshot);
        }
    }
    for (const char* managed : {sandbox::kRememberedRulesFile, sandbox::kRememberedSandboxedRulesFile}) {
        if (by_name.count(managed)) continue;
        ExecRulesFileSnapshot snapshot;
        snapshot.name = managed;
        snapshot.path = path_to_utf8(root / managed);
        snapshot.managed = true;
        snapshot.scope = sandbox::rules_file_scope(managed);
        by_name[managed] = std::move(snapshot);
    }
    std::vector<ExecRulesFileSnapshot> out;
    out.reserve(by_name.size());
    // 托管文件排前面(default.rules、default.sandboxed.rules),其余按名。
    for (const char* managed : {sandbox::kRememberedRulesFile, sandbox::kRememberedSandboxedRulesFile}) {
        auto it = by_name.find(managed);
        if (it != by_name.end()) {
            out.push_back(std::move(it->second));
            by_name.erase(it);
        }
    }
    for (auto& [name, snapshot] : by_name) {
        (void)name;
        out.push_back(std::move(snapshot));
    }
    return out;
}

json prefix_rule_to_json(const sandbox::PrefixRule& rule) {
    json pattern = json::array();
    for (const auto& alternatives : rule.pattern) {
        if (alternatives.size() == 1) pattern.push_back(alternatives[0]);
        else pattern.push_back(alternatives);
    }
    const auto decision = rule.decision == sandbox::RuleDecision::AllowSandboxed
        ? sandbox::RuleDecision::Allow : rule.decision;
    return json{
        {"pattern", pattern},
        {"display", pattern_display(rule.pattern)},
        {"decision", sandbox::rule_decision_name(decision)},
        {"justification", rule.justification},
    };
}

json exec_rules_snapshot(const std::string& dir, const std::vector<ExecRulesFileSnapshot>& files) {
    json list = json::array();
    for (const auto& file : files) {
        json rules = json::array();
        for (const auto& rule : file.rules) rules.push_back(prefix_rule_to_json(rule));
        list.push_back(json{
            {"name", file.name},
            {"path", file.path},
            {"managed", file.managed},
            {"scope", file.scope == sandbox::RuleScope::Sandboxed ? "sandboxed" : "global"},
            {"exists", file.exists},
            {"error", file.error},
            {"rules", rules},
        });
    }
    return json{
        {"dir", dir},
        {"managed", json::array({sandbox::kRememberedRulesFile, sandbox::kRememberedSandboxedRulesFile})},
        {"files", list},
    };
}

bool parse_exec_rules_request(const json& body,
                              std::map<std::string, std::vector<sandbox::PrefixRule>>& out,
                              std::string& error) {
    if (!body.is_object() || !body.contains("files") || !body["files"].is_object()) {
        error = "body must be {files:{name:[rules]}}";
        return false;
    }
    std::map<std::string, std::vector<sandbox::PrefixRule>> parsed;
    for (const auto& [name, rules_json] : body["files"].items()) {
        if (!sandbox::is_managed_rules_file(name)) {
            error = "only managed rules files can be written: " + name;
            return false;
        }
        if (!rules_json.is_array()) {
            error = name + " must be an array of rules";
            return false;
        }
        const auto scope = sandbox::rules_file_scope(name);
        std::vector<sandbox::PrefixRule> rules;
        for (const auto& item : rules_json) {
            if (!item.is_object()) {
                error = name + ": each rule must be an object";
                return false;
            }
            sandbox::PrefixRule rule;
            rule.scope = scope;
            rule.source_file = name;
            std::string pattern_error;
            if (!item.contains("pattern") || !parse_pattern(item["pattern"], rule.pattern, pattern_error)) {
                error = name + ": " + (pattern_error.empty() ? "pattern is required" : pattern_error);
                return false;
            }
            const std::string decision = item.value("decision", std::string("allow"));
            if (decision == "allow") rule.decision = sandbox::RuleDecision::Allow;
            else if (decision == "prompt") rule.decision = sandbox::RuleDecision::Prompt;
            else if (decision == "forbidden") rule.decision = sandbox::RuleDecision::Forbidden;
            else {
                error = name + ": decision must be allow, prompt or forbidden";
                return false;
            }
            if (scope == sandbox::RuleScope::Sandboxed && rule.decision != sandbox::RuleDecision::Allow) {
                error = name + ": the sandboxed file only holds allow rules (put prompt / forbidden rules in " +
                        std::string(sandbox::kRememberedRulesFile) + ")";
                return false;
            }
            if (item.contains("justification")) {
                if (!item["justification"].is_string()) {
                    error = name + ": justification must be a string";
                    return false;
                }
                rule.justification = trim_copy(item["justification"].get<std::string>());
            }
            if (rule.decision == sandbox::RuleDecision::Allow) {
                // 放行前缀走与「批准并记住」相同的禁用名单(整条 pattern 精确比较:
                // `git` 单独被禁是因为太宽,`git push --force` 却是合法规则),另外首 token
                // 是解释器 / shell / rm / sudo 这类能承载任意行为的命令时,不管后面跟什么
                // 都拒绝 —— `rm -rf` 放行的是任意 `rm -rf <路径>`。首 token 候选并集逐个检查。
                // 想放行 `python manage.py test` 这类命令的用户仍可手写非托管 *.rules。
                for (const auto& head : rule.pattern.front()) {
                    std::vector<std::string> tokens{head};
                    for (std::size_t i = 1; i < rule.pattern.size(); ++i) tokens.push_back(rule.pattern[i].front());
                    if (sandbox::is_banned_prefix(tokens) || is_dangerous_allow_head(head)) {
                        error = name + ": `" + pattern_display(rule.pattern) +
                                "` cannot be an allow rule (interpreters, shells, rm, sudo and similar prefixes are banned)";
                        return false;
                    }
                }
            }
            rules.push_back(std::move(rule));
        }
        parsed[name] = std::move(rules);
    }
    out = std::move(parsed);
    error.clear();
    return true;
}

// ---------------------------------------------------------------------------
// 审计
// ---------------------------------------------------------------------------

bool parse_audit_query(const std::map<std::string, std::string>& params,
                       security::AuditQuery& out, std::string& error) {
    security::AuditQuery query;
    query.limit = 100;
    auto get = [&](const char* key) -> std::string {
        auto it = params.find(key);
        return it == params.end() ? std::string{} : trim_copy(it->second);
    };
    query.category = get("category");
    if (!query.category.empty() && !known_categories().count(query.category)) {
        error = "unknown category: " + query.category;
        return false;
    }
    query.decision = get("decision");
    if (!query.decision.empty() && !known_decisions().count(query.decision)) {
        error = "unknown decision: " + query.decision;
        return false;
    }
    query.text = get("q");
    if (const std::string since = get("since_ms"); !since.empty()) {
        std::int64_t value = 0;
        if (!parse_i64(since, value) || value < 0) {
            error = "since_ms must be a non-negative integer";
            return false;
        }
        query.since_ms = value;
    }
    if (const std::string before = get("before_id"); !before.empty()) {
        std::int64_t value = 0;
        if (!parse_i64(before, value) || value < 0) {
            error = "before_id must be a non-negative integer";
            return false;
        }
        query.before_id = value;
    }
    if (const std::string limit = get("limit"); !limit.empty()) {
        std::int64_t value = 0;
        if (!parse_i64(limit, value) || value < 1 || value > 500) {
            error = "limit must be between 1 and 500";
            return false;
        }
        query.limit = static_cast<int>(value);
    }
    out = std::move(query);
    error.clear();
    return true;
}

json audit_page_to_json(const security::AuditPage& page) {
    json entries = json::array();
    for (const auto& entry : page.entries) entries.push_back(security::audit_entry_to_json(entry));
    json out{
        {"entries", entries},
        {"has_more", page.has_more},
        {"total", page.total},
    };
    if (page.has_more && !page.entries.empty()) out["next_before_id"] = page.entries.back().id;
    return out;
}

json audit_summary_to_json(const security::AuditSummary& summary) {
    json blocked = json::array();
    for (const auto& item : summary.blocked_paths) {
        blocked.push_back(json{{"path", item.path}, {"count", item.count}, {"last_ts_ms", item.last_ts_ms}});
    }
    return json{
        {"total", summary.total},
        {"by_decision", summary.by_decision},
        {"by_category", summary.by_category},
        {"last_ts_ms", summary.last_ts_ms},
        {"blocked_paths", blocked},
    };
}

std::string audit_export_filename(const std::string& format, std::int64_t now_ms) {
    const std::time_t seconds = static_cast<std::time_t>(now_ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string("acecode-audit-") + stamp + (format == "csv" ? ".csv" : ".jsonl");
}

} // namespace acecode::web
