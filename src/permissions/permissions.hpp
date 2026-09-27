#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <optional>
#include <algorithm>
#include <cstring>
#include <atomic>

namespace acecode {

// Rule action for the permission rules engine
enum class RuleAction { Allow, Deny };

// A permission rule that matches tool calls by tool name, path, and/or command pattern
struct PermissionRule {
    std::string tool_pattern;    // glob: "bash", "file_*", "*"
    std::string path_pattern;    // glob: "src/**", "*.env", "" (any)
    std::string command_pattern; // prefix: "git ", "rm -rf /", "" (any)
    RuleAction action = RuleAction::Allow;
    int priority = 0;            // higher = evaluated first
};

// Permission mode for the current session
enum class PermissionMode {
    Default,      // Prompt for write/exec tools, auto-allow read-only
    // Auto(openspec add-auto-mode-sandbox,复刻 Codex 的 Auto 预设,取代原
    // accept-edits):文件编辑自动放行;bash 走 src/sandbox 的决策表 ——
    // 已知安全命令与沙盒内的未知命令直接跑,危险命令 / 越权申请 / 无沙盒时
    // 的未知命令才确认。老名字 accept-edits / acceptEdits 仍可解析。
    Auto,
    Yolo,         // Auto-allow all tool permissions without prompting (no sandbox)
    Plan          // Explore and write only the active plan file before approval
};

// Result of a permission check
enum class PermissionResult {
    Allow,        // Execute the tool
    Deny,         // Reject this tool call
    AlwaysAllow,  // Allow + remember for this tool for the rest of the session
    // bash 专属(openspec align-codex-sandboxing D5):
    AllowScoped,  // 只放行确认框建议的那个目录(会话授权),命令留在沙盒里执行
    AllowRemember // Allow + 把命令前缀写进规则文件,以后不再问
};

// bash 的会话级「总是允许」记忆结论(按命令前缀,见 add_session_command_allow)。
enum class SessionCommandAllow {
    None,       // 没记过
    Sandboxed,  // 记过:免确认,但仍走模式沙盒
    Bypass      // 记过且当时批准的是越权申请:免确认,沙盒外执行
};

// Manages tool permission decisions
class PermissionManager {
public:
    PermissionManager() {
        // 所有宿主与子会话共用内置保护,不能由模型自行写入免确认规则。
        for (const char* tool : {"file_write", "file_edit", "apply_patch"}) {
            for (const char* path : {".acecode/rules/**", "**/.acecode/rules/**"}) {
                rules_.push_back({tool, path, "", RuleAction::Deny, 1000});
            }
        }
    }
    void set_mode(PermissionMode mode) {
        const PermissionMode current = mode_.load(std::memory_order_relaxed);
        if (mode == PermissionMode::Plan) {
            if (current != PermissionMode::Plan) {
                pre_plan_mode_.store(current, std::memory_order_relaxed);
                has_pre_plan_mode_.store(true, std::memory_order_relaxed);
            }
        } else {
            has_pre_plan_mode_.store(false, std::memory_order_relaxed);
        }
        mode_.store(mode, std::memory_order_relaxed);
        if (current != mode) clear_session_allows();
    }
    PermissionMode mode() const { return mode_.load(std::memory_order_relaxed); }

    PermissionMode pre_plan_mode() const {
        if (!has_pre_plan_mode_.load(std::memory_order_relaxed)) {
            return PermissionMode::Default;
        }
        PermissionMode mode = pre_plan_mode_.load(std::memory_order_relaxed);
        return mode == PermissionMode::Plan ? PermissionMode::Default : mode;
    }

    void set_pre_plan_mode(PermissionMode mode) {
        if (mode == PermissionMode::Plan) mode = PermissionMode::Default;
        pre_plan_mode_.store(mode, std::memory_order_relaxed);
        has_pre_plan_mode_.store(true, std::memory_order_relaxed);
    }

    PermissionMode restore_pre_plan_mode() {
        PermissionMode restored = pre_plan_mode();
        mode_.store(restored, std::memory_order_relaxed);
        has_pre_plan_mode_.store(false, std::memory_order_relaxed);
        clear_session_allows();
        return restored;
    }

    // Enable/disable dangerous mode (bypasses ALL checks including path safety)
    void set_dangerous(bool enabled) { dangerous_mode_.store(enabled, std::memory_order_relaxed); }
    bool is_dangerous() const { return dangerous_mode_.load(std::memory_order_relaxed); }

    // Add a permission rule
    void add_rule(const PermissionRule& rule) {
        std::lock_guard<std::mutex> lk(mu_);
        rules_.push_back(rule);
    }

    // 命中的最高优先级规则(同优先级 Deny 胜)。priority >= kBuiltinProtectionPriority
    // 的是构造函数登记的内置保护规则(`.acecode/rules/**`),AgentLoop 只对它们和
    // yolo 模式硬拒绝;配置里的普通 Deny(`.env` / `.git/**`)回到弹确认
    // (openspec align-codex-sandboxing D10)。
    static constexpr int kBuiltinProtectionPriority = 1000;

    std::optional<PermissionRule> matched_rule_detail(const std::string& tool_name,
                                                      const std::string& path = "",
                                                      const std::string& command = "") const {
        std::lock_guard<std::mutex> lk(mu_);
        const PermissionRule* best = nullptr;
        for (const auto& rule : rules_) {
            if (rule_matches(rule, tool_name, path, command) &&
                (!best || rule.priority > best->priority ||
                 (rule.priority == best->priority && rule.action == RuleAction::Deny))) {
                best = &rule;
            }
        }
        return best ? std::optional<PermissionRule>(*best) : std::nullopt;
    }

    std::optional<RuleAction> matched_rule(const std::string& tool_name,
                                           const std::string& path = "",
                                           const std::string& command = "") const {
        const auto detail = matched_rule_detail(tool_name, path, command);
        return detail ? std::optional<RuleAction>(detail->action) : std::nullopt;
    }

    // Check if a tool should be auto-allowed (no user prompt needed)
    // path and command are optional context for rule matching
    bool should_auto_allow(const std::string& tool_name, bool is_read_only,
                           const std::string& path = "",
                           const std::string& command = "") const {
        // Dangerous mode: everything is auto-allowed unconditionally
                if (dangerous_mode_.load(std::memory_order_relaxed)) return true;

        // Check rules (priority-ordered, first match wins)
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!rules_.empty()) {
                // Build sorted copy by descending priority
                auto sorted = rules_;
                std::sort(sorted.begin(), sorted.end(),
                    [](const PermissionRule& a, const PermissionRule& b) {
                        return a.priority > b.priority;
                    });
                for (const auto& rule : sorted) {
                    if (rule_matches(rule, tool_name, path, command)) {
                        if (rule.action == RuleAction::Deny) return false;
                        if (rule.action == RuleAction::Allow) return true;
                    }
                }
            }
        }

        // Yolo mode: tools are auto-allowed by default. An explicitly matched
        // Deny rule returned false above; AgentLoop treats that as a silent
        // rejection instead of opening a permission prompt.
        if (mode_.load(std::memory_order_relaxed) == PermissionMode::Yolo) return true;

        // Read-only tools are always auto-allowed
        if (is_read_only) return true;

        // memory_write is auto-allowed in every non-Yolo mode because its
        // tool implementation hard-locks the target path to
        // ~/.acecode/memory/ and rejects anything else — the PermissionManager
        // doesn't need to prompt on top of that.
        if (tool_name == "memory_write") return true;

        // task_complete is a zero-side-effect terminator signal. Even though
        // its ToolImpl already sets is_read_only=true (so the read-only branch
        // above would catch it), we also name-match here so future refactors
        // that toggle that flag can't accidentally turn task_complete into a
        // tool that prompts the user mid-loop.
        if (tool_name == "task_complete") return true;

        // TodoWrite mutates only session-local checklist state so it must run
        // sequentially, but it should never interrupt the user with a file/exec
        // permission prompt.
        if (tool_name == "TodoWrite") return true;

        // Native Agent Browser tools operate only on ACECode's isolated,
        // user-visible browser page. Keep mutating browser work sequential
        // without prompting before every pointer or navigation action.
        static constexpr const char* kNativeAgentBrowserTools[] = {
            "browser_open",
            "browser_navigate",
            "browser_read_page",
            "browser_click",
            "browser_fill",
            "browser_type",
            "browser_press",
            "browser_hover",
            "browser_drag",
            "browser_scroll",
            "browser_wait",
            "browser_screenshot",
            "browser_handle_dialog",
            "browser_evaluate",
            "browser_close",
        };
        for (const char* browser_tool : kNativeAgentBrowserTools) {
            if (tool_name == browser_tool) return true;
        }

        // Session-level always-allow for this tool
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (session_allowed_.count(tool_name)) return true;
        }

        // Auto mode: auto-allow file tools (apply_patch is the GPT-family
        // spelling of file_edit). bash is decided by the exec policy in
        // AgentLoop (src/sandbox/exec_decision), never here.
        if (mode_.load(std::memory_order_relaxed) == PermissionMode::Auto) {
            if (tool_name == "file_write" || tool_name == "file_edit" ||
                tool_name == "apply_patch") {
                return true;
            }
        }

        return false;
    }

    // Record that user chose "always allow" for a tool
    void add_session_allow(const std::string& tool_name) {
        if (tool_name == "bash") return;
        std::lock_guard<std::mutex> lk(mu_);
        session_allowed_.insert(tool_name);
    }

    // Check if a tool has session-level always-allow
    bool has_session_allow(const std::string& tool_name) const {
        std::lock_guard<std::mutex> lk(mu_);
        return session_allowed_.count(tool_name) > 0;
    }

    // bash 的「总是允许」按命令前缀记(`git commit` / `pnpm test`),而不是整个
    // 工具:点一次"总是允许"不该等于本会话所有 shell 命令全放行。bypass=true
    // 表示当时批准的是越权申请,同前缀后续命令在沙盒外执行。
    void add_session_command_allow(const std::string& prefix, bool bypass_sandbox) {
        if (prefix.empty()) return;
        std::lock_guard<std::mutex> lk(mu_);
        bool& slot = session_command_allowed_[prefix];
        slot = slot || bypass_sandbox;
    }

    // 多段命令逐段查:每段都记过才算记过;全部带 bypass 才是 Bypass。
    SessionCommandAllow session_command_allow(const std::vector<std::string>& prefixes) const {
        if (prefixes.empty()) return SessionCommandAllow::None;
        std::lock_guard<std::mutex> lk(mu_);
        bool all_bypass = true;
        for (const auto& prefix : prefixes) {
            auto it = session_command_allowed_.find(prefix);
            if (it == session_command_allowed_.end()) return SessionCommandAllow::None;
            if (!it->second) all_bypass = false;
        }
        return all_bypass ? SessionCommandAllow::Bypass : SessionCommandAllow::Sandboxed;
    }

    std::vector<std::string> session_command_allows() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> out;
        for (const auto& [prefix, bypass] : session_command_allowed_) {
            out.push_back(bypass ? prefix + " (bypass sandbox)" : prefix);
        }
        return out;
    }

    // Clear all session allows (e.g., on mode change)
    void clear_session_allows() {
        std::lock_guard<std::mutex> lk(mu_);
        session_allowed_.clear();
        session_command_allowed_.clear();
    }

    // Cycle to next permission mode
    PermissionMode cycle_mode() {
        PermissionMode next = PermissionMode::Default;
        switch (mode_.load(std::memory_order_relaxed)) {
            case PermissionMode::Default: next = PermissionMode::Auto; break;
            case PermissionMode::Auto:    next = PermissionMode::Yolo; break;
            case PermissionMode::Yolo:    next = PermissionMode::Plan; break;
            case PermissionMode::Plan:    next = PermissionMode::Default; break;
        }
        set_mode(next);
        clear_session_allows();
        return next;
    }

    static const char* mode_name(PermissionMode m) {
        switch (m) {
            case PermissionMode::Default: return "default";
            case PermissionMode::Auto:    return "auto";
            case PermissionMode::Yolo:    return "yolo";
            case PermissionMode::Plan:    return "plan";
        }
        return "unknown";
    }

    // 模式名解析的唯一入口:别名(accept-edits / acceptEdits = auto)只在这里
    // 维护。返回 nullopt = 未知名字,调用方自己决定报错还是回退。
    static std::optional<PermissionMode> parse_mode_name(std::string name) {
        // 去两端空白。
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t' ||
                                 name.back() == '\r' || name.back() == '\n')) {
            name.pop_back();
        }
        std::size_t start = 0;
        while (start < name.size() && (name[start] == ' ' || name[start] == '\t')) ++start;
        name = name.substr(start);
        if (name == "default") return PermissionMode::Default;
        if (name == "auto" || name == "accept-edits" || name == "acceptEdits") {
            return PermissionMode::Auto;
        }
        if (name == "yolo") return PermissionMode::Yolo;
        if (name == "plan") return PermissionMode::Plan;
        return std::nullopt;
    }

    // 解析后再序列化:把别名归一成线上名("accept-edits" → "auto"),未知名原样返回。
    static std::string canonical_mode_name(const std::string& name) {
        if (auto mode = parse_mode_name(name)) return mode_name(*mode);
        return name;
    }

    static const char* mode_description(PermissionMode m) {
        switch (m) {
            case PermissionMode::Default: return "Prompt for write/exec tools";
            case PermissionMode::Auto:    return "Auto-run edits and sandboxed commands; ask when leaving the workspace";
            case PermissionMode::Yolo:    return "Auto-allow every tool without prompting (no sandbox)";
            case PermissionMode::Plan:    return "Plan first, approve before coding";
        }
        return "";
    }

private:
    // Simple glob match: supports * (any non-/) and ** (any including /)
    // Empty pattern matches everything
    static bool glob_match(const std::string& pattern, const std::string& text) {
        if (pattern.empty()) return true;
        return glob_match_impl(pattern.c_str(), text.c_str());
    }

    static bool glob_match_impl(const char* p, const char* t) {
        while (*p) {
            if (p[0] == '*' && p[1] == '*') {
                // ** matches any sequence including /
                p += 2;
                if (*p == '/') p++; // skip optional / after **
                if (!*p) return true;
                for (const char* s = t; *s; s++) {
                    if (glob_match_impl(p, s)) return true;
                }
                return glob_match_impl(p, t + strlen(t)); // match empty
            }
            if (*p == '*') {
                // * matches any sequence except /
                p++;
                for (const char* s = t; *s && *s != '/'; s++) {
                    if (glob_match_impl(p, s)) return true;
                }
                return glob_match_impl(p, t); // * can match empty
            }
            if (*p == '?') {
                if (!*t || *t == '/') return false;
                p++; t++;
                continue;
            }
            // Case-insensitive char compare
            char pc = *p, tc = *t;
            if (pc >= 'A' && pc <= 'Z') pc += 32;
            if (tc >= 'A' && tc <= 'Z') tc += 32;
            if (pc != tc) return false;
            p++; t++;
        }
        return *t == '\0';
    }

    // Normalize path separators to /
    static std::string normalize_path(const std::string& path) {
        std::string result = path;
        for (auto& c : result) {
            if (c == '\\') c = '/';
        }
        return result;
    }

    // Check if a rule matches the given context
    static bool rule_matches(const PermissionRule& rule,
                             const std::string& tool_name,
                             const std::string& path,
                             const std::string& command) {
        // Tool pattern must match
        if (!rule.tool_pattern.empty() && !glob_match(rule.tool_pattern, tool_name)) {
            return false;
        }
        // Path pattern must match (if rule has one and path is provided)
        if (!rule.path_pattern.empty() && !path.empty()) {
            std::string norm = normalize_path(path);
            if (!glob_match(rule.path_pattern, norm)) return false;
        } else if (!rule.path_pattern.empty() && path.empty()) {
            return false; // rule requires path but none provided
        }
        // Command pattern: prefix match
        if (!rule.command_pattern.empty() && !command.empty()) {
            if (command.substr(0, rule.command_pattern.size()) != rule.command_pattern) {
                return false;
            }
        } else if (!rule.command_pattern.empty() && command.empty()) {
            return false; // rule requires command but none provided
        }
        return true;
    }

    std::atomic<PermissionMode> mode_{PermissionMode::Default};
    std::atomic<PermissionMode> pre_plan_mode_{PermissionMode::Default};
    std::atomic<bool> has_pre_plan_mode_{false};
    std::atomic<bool> dangerous_mode_{false};
    mutable std::mutex mu_;
    std::set<std::string> session_allowed_;
    std::map<std::string, bool> session_command_allowed_;   // prefix → bypass_sandbox
    std::vector<PermissionRule> rules_;
};

} // namespace acecode
