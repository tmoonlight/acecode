#include "session_exec_security.hpp"

#include "agent/boundary/workspace_boundary.hpp"
#include "config/config.hpp"
#include "session/session_manager.hpp"
#include "utils/logger.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <utility>

namespace acecode::agent {

void SessionExecSecurity::set_rules(sandbox::ExecRules rules) {
    std::lock_guard<std::mutex> lock(state_mu_);
    rules_ = std::move(rules);
}

void SessionExecSecurity::set_rules_dir(std::string dir) {
    { std::lock_guard<std::mutex> lock(state_mu_); rules_dir_ = std::move(dir); }
    reload_exec_rules();
}

void SessionExecSecurity::set_audit_sink(security::AuditSink sink) {
    std::lock_guard<std::mutex> lock(state_mu_);
    audit_sink_ = std::move(sink);
}

std::optional<sandbox::SandboxViolation> SessionExecSecurity::feedback() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return feedback_;
}

void SessionExecSecurity::set_feedback(std::optional<sandbox::SandboxViolation> feedback) {
    std::lock_guard<std::mutex> lock(state_mu_);
    feedback_ = std::move(feedback);
}

void SessionExecSecurity::reset_prompt_snapshot() {
    std::lock_guard<std::mutex> lock(prompt_mu_);
    prompt_snapshot_.reset();
}

sandbox::ExecPermission SessionExecSecurity::evaluate_exec(
    const std::string& args, sandbox::CommandPlatform platform,
    const sandbox::ExecPermissionOptions& options) {
    sandbox::ExecRules rules;
    { std::lock_guard<std::mutex> lock(state_mu_); rules = rules_; }
    return sandbox::evaluate_exec_permission(args, permissions_, rules,
        !session_disabled_ && runtime_.available(), platform, options);
}

std::string SessionExecSecurity::global_exec_rules_dir() const {
    { std::lock_guard<std::mutex> lock(state_mu_); if (!rules_dir_.empty()) return rules_dir_; }
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "rules");
}

void SessionExecSecurity::reload_exec_rules() {
    set_rules(sandbox::ExecRules::load(
        global_exec_rules_dir(),
        path_to_utf8(path_from_utf8(boundary_.cwd()) / ".acecode" / "rules")));
}

std::string SessionExecSecurity::remember_exec_rule(const sandbox::ExecPermission& permission) {
    if (permission.remember_patterns.empty()) return "no command prefix to remember";
    // 沙盒外批准 → default.rules(全局 allow = 沙盒外);其余 → default.sandboxed.rules
    // (免确认但仍沙盒)。与会话前缀记忆的 bypass 判定同一条件。
    const bool bypass = permission.decision.sandbox == sandbox::SandboxMode::FullAccess &&
                        permission.input.escalation_requested;
    const auto file = path_from_utf8(global_exec_rules_dir()) /
                      (bypass ? sandbox::kRememberedRulesFile : sandbox::kRememberedSandboxedRulesFile);
    const std::string error = sandbox::append_prefix_rules(path_to_utf8(file), permission.remember_patterns);
    if (!error.empty()) {
        LOG_WARN("[sandbox] cannot remember exec rule: " + error);
        return error;
    }
    LOG_INFO("[sandbox] remembered exec rule in " + path_to_utf8(file) + ": " + permission.remember_display());
    reload_exec_rules();
    return {};
}

void SessionExecSecurity::record_audit(SessionManager* session, const std::string& category, const std::string& tool,
                             const std::string& target, const std::string& decision,
                             const std::string& source, const std::string& reason,
                             const std::string& sandbox, nlohmann::json detail) {
    try {
        security::AuditEntry entry;
        entry.ts_ms = security::audit_now_ms();
        entry.category = category;
        entry.tool = tool;
        // 命令原文可能很长(heredoc 写文件),存 4000 字符够看清是什么,别把日志撑爆。
        entry.target = target.size() > 4000 ? target.substr(0, 4000) + "…" : target;
        entry.decision = decision;
        entry.source = source;
        entry.reason = reason;
        entry.sandbox = sandbox;
        entry.session_id = session ? session->current_session_id() : std::string{};
        entry.cwd = boundary_.cwd();
        entry.detail = detail.is_object() ? std::move(detail) : nlohmann::json::object();
        security::AuditSink sink;
        { std::lock_guard<std::mutex> lock(state_mu_); sink = audit_sink_; }
        if (sink) {
            sink(entry);
        } else {
            security::audit_log().record(entry);
        }
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[audit] record failed: ") + e.what());
    } catch (...) {
        LOG_WARN("[audit] record failed");
    }
}

void SessionExecSecurity::set_sandbox_config(const SandboxConfig& config) {
    sandbox::SandboxRuntimeConfig runtime_config;
    runtime_config.enabled = config.enabled;
    runtime_config.network_access = config.network_access;
    runtime_config.writable_roots = config.writable_roots;
    for (const auto& entry : config.filesystem_write) runtime_config.writable_roots.push_back(entry);
    runtime_config.exclude_tmpdir = config.exclude_tmpdir;
    runtime_config.readable_roots = config.filesystem_read;
    runtime_config.denied_entries = config.filesystem_deny;
    runtime_config.deny_defaults = config.deny_defaults;
    runtime_config.windows_backend = config.windows_backend == "mxc"
        ? sandbox::WindowsBackendChoice::Mxc : sandbox::WindowsBackendChoice::RestrictedToken;
    runtime_config.acecode_home = get_acecode_dir();
    runtime_.configure(std::move(runtime_config));
    permissions_.clear_session_allows();
    set_feedback(std::nullopt);
}

std::string SessionExecSecurity::sandbox_prompt_description(SessionManager* session) const {
    std::lock_guard<std::mutex> lock(prompt_mu_);
    const auto permission_mode = permissions_.mode();
    if (busy_ && prompt_snapshot_ && prompt_snapshot_->first == permission_mode) {
        return prompt_snapshot_->second;
    }
    const auto describe = [&]() -> std::string {
    if (permissions_.is_dangerous() || permissions_.mode() == PermissionMode::Yolo) return "none";
    if (session_disabled_) return "unavailable (disabled for this session)";
    const auto probe = runtime_.probe();
    if (!runtime_.available()) return "unavailable (" + probe.reason + ")";
    const auto mode = sandbox::mode_sandbox(permissions_.mode(), true);
    const auto root = boundary_.write_root(session).empty() ? boundary_.cwd() : boundary_.write_root(session);
    return std::string(sandbox::sandbox_mode_name(mode)) + " (" + sandbox::backend_kind_name(probe.kind) +
        "); " + sandbox::describe_policy(runtime_.policy_for(mode, root), probe.network_enforced);
    };
    auto result = describe();
    if (busy_) prompt_snapshot_ = std::make_pair(permission_mode, result);
    return result;
}

std::string SessionExecSecurity::sandbox_command(SessionManager* session, const std::string& args) {
    if (args == "off" || args == "on") {
        session_disabled_.store(args == "off");
        permissions_.clear_session_allows();
        runtime_.clear_session_grants();
        set_feedback(std::nullopt);
        // `on` 同时丢掉本会话缓存的探测结论:prepare_request / 启动失败会经
        // mark_unavailable 把后端粘性地标成不可用,用户修好环境(比如把网络盘
        // 上的工作区挪回本地)之后需要一个不重启的恢复入口。
        if (args == "on") runtime_.reset_probe();
    } else if (!args.empty()) {
        return "Usage: /sandbox [on|off]";
    }
    return runtime_.status_text(permissions_.mode(),
        boundary_.write_root(session).empty() ? boundary_.cwd() : boundary_.write_root(session), session_disabled_);
}

} // namespace acecode::agent
