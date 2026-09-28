#include "agent/agent_loop.hpp"
#include "agent/guards/doom_guard.hpp"
#include "computer_use/runtime.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_runtime.hpp"
#include "pa/pa_context_budget.hpp"
#include "permissions/shell_write_guard.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "sandbox/exec_permission.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"
#include "session/token_tracker.hpp"
#include "session/turn_timing.hpp"
#include "skills/skill_registry.hpp"
#include "skills/skill_usage_store.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "utils/text.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

std::string AgentLoop::global_exec_rules_dir() const {
    if (!exec_rules_dir_override_.empty()) return exec_rules_dir_override_;
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "rules");
}

void AgentLoop::reload_exec_rules() {
    exec_rules_ = sandbox::ExecRules::load(
        global_exec_rules_dir(),
        path_to_utf8(path_from_utf8(cwd_) / ".acecode" / "rules"));
}

std::string AgentLoop::remember_exec_rule(const sandbox::ExecPermission& permission) {
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

void AgentLoop::record_audit(const std::string& category, const std::string& tool,
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
        entry.session_id = session_manager_ ? session_manager_->current_session_id() : std::string{};
        entry.cwd = cwd_;
        entry.detail = detail.is_object() ? std::move(detail) : nlohmann::json::object();
        if (audit_sink_) {
            audit_sink_(entry);
        } else {
            security::audit_log().record(entry);
        }
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[audit] record failed: ") + e.what());
    } catch (...) {
        LOG_WARN("[audit] record failed");
    }
}

void AgentLoop::set_sandbox_config(const SandboxConfig& config) {
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
    sandbox_runtime_.configure(std::move(runtime_config));
    permissions_.clear_session_allows();
    last_sandbox_violation_.reset();
}

std::string AgentLoop::sandbox_prompt_description() const {
    std::lock_guard<std::mutex> lock(sandbox_prompt_mutex_);
    const auto permission_mode = permissions_.mode();
    if (busy_ && sandbox_prompt_snapshot_ && sandbox_prompt_snapshot_->first == permission_mode) {
        return sandbox_prompt_snapshot_->second;
    }
    const auto describe = [&]() -> std::string {
    if (permissions_.is_dangerous() || permissions_.mode() == PermissionMode::Yolo) return "none";
    if (sandbox_session_disabled_) return "unavailable (disabled for this session)";
    const auto probe = sandbox_runtime_.probe();
    if (!sandbox_runtime_.available()) return "unavailable (" + probe.reason + ")";
    const auto mode = sandbox::mode_sandbox(permissions_.mode(), true);
    const auto root = write_root().empty() ? cwd_ : write_root();
    return std::string(sandbox::sandbox_mode_name(mode)) + " (" + sandbox::backend_kind_name(probe.kind) +
        "); " + sandbox::describe_policy(sandbox_runtime_.policy_for(mode, root), probe.network_enforced);
    };
    auto result = describe();
    if (busy_) sandbox_prompt_snapshot_ = std::make_pair(permission_mode, result);
    return result;
}

std::string AgentLoop::sandbox_command(const std::string& args) {
    if (args == "off" || args == "on") {
        sandbox_session_disabled_.store(args == "off");
        permissions_.clear_session_allows();
        sandbox_runtime_.clear_session_grants();
        last_sandbox_violation_.reset();
        // `on` 同时丢掉本会话缓存的探测结论:prepare_request / 启动失败会经
        // mark_unavailable 把后端粘性地标成不可用,用户修好环境(比如把网络盘
        // 上的工作区挪回本地)之后需要一个不重启的恢复入口。
        if (args == "on") sandbox_runtime_.reset_probe();
    } else if (!args.empty()) {
        return "Usage: /sandbox [on|off]";
    }
    return sandbox_runtime_.status_text(permissions_.mode(),
        write_root().empty() ? cwd_ : write_root(), sandbox_session_disabled_);
}

} // namespace acecode
