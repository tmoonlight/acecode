#pragma once
#include "agent/agent_runtime_env.hpp"

#include "agent/approval/auto_review_gate.hpp"
#include "agent/approval/safe_edit_guard.hpp"
#include "permissions/auto_review_denials.hpp"
#include "sandbox/exec_permission.hpp"
#include "sandbox/sandbox_denial.hpp"
#include "sandbox/sandbox_runtime.hpp"
#include "security/audit_log.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace acecode { struct SandboxConfig; class SessionManager; }

namespace acecode::agent {
class WorkspaceBoundary;

// Runtime, rules and feedback belong to one session. Leaf locks protect value
// snapshots only; filesystem access, audit sinks and collaborators run outside.
class SessionExecSecurity {
public:
    SessionExecSecurity(WorkspaceBoundary& boundary, PermissionManager& permissions,
                        const std::atomic<bool>& busy, AgentRuntimeEnv environment = {})
        : boundary_(boundary), permissions_(permissions), busy_(busy), environment_(std::move(environment)) {}
    const AgentRuntimeEnv& environment() const { return environment_; }
    sandbox::SandboxRuntime& runtime() const { return runtime_; }
    bool session_disabled() const { return session_disabled_.load(); }
    void set_rules(sandbox::ExecRules rules);
    void set_rules_dir(std::string dir);
    void reload_exec_rules();
    std::string remember_exec_rule(const sandbox::ExecPermission& permission);
    sandbox::ExecPermission evaluate_exec(const std::string& args, sandbox::CommandPlatform platform,
                                          const sandbox::ExecPermissionOptions& options);
    void set_audit_sink(security::AuditSink sink);
    void record_audit(SessionManager* session, const std::string& category, const std::string& tool,
                      const std::string& target, const std::string& decision,
                      const std::string& source, const std::string& reason,
                      const std::string& sandbox, nlohmann::json detail);
    void set_sandbox_config(const SandboxConfig& config);
    std::string sandbox_prompt_description(SessionManager* session) const;
    std::string sandbox_command(SessionManager* session, const std::string& args);
    void reset_prompt_snapshot();
    std::optional<sandbox::SandboxViolation> feedback() const;
    void set_feedback(std::optional<sandbox::SandboxViolation> feedback);
    SafeEditGuard& safe_edit_guard() { return safe_edit_guard_; }

    // 自动模式（试验）(openspec add-auto-review-mode)的会话级状态:拦截计数跨工具
    // 批次存活;裁决模型由宿主提供的取得函数给出(空 = 没配置,用会话当前模型)。
    using ReviewProviderAccessor = std::function<std::shared_ptr<LlmProvider>()>;
    auto_review::DenialTracker& review_denials() { return review_denials_; }
    void set_review_provider(ReviewProviderAccessor accessor);
    std::shared_ptr<LlmProvider> review_provider() const;
    ReviewTimeouts review_timeouts() const;
    void set_review_timeouts_for_tests(ReviewTimeouts timeouts);
private:
    std::string global_exec_rules_dir() const;
    WorkspaceBoundary& boundary_;
    PermissionManager& permissions_;
    const std::atomic<bool>& busy_;
    AgentRuntimeEnv environment_;
    mutable sandbox::SandboxRuntime runtime_;
    std::atomic<bool> session_disabled_{false};
    mutable std::mutex state_mu_;
    sandbox::ExecRules rules_;
    std::string rules_dir_;
    security::AuditSink audit_sink_;
    std::optional<sandbox::SandboxViolation> feedback_;
    mutable std::mutex prompt_mu_;
    mutable std::optional<std::pair<PermissionMode, std::string>> prompt_snapshot_;
    SafeEditGuard safe_edit_guard_;
    auto_review::DenialTracker review_denials_;
    ReviewProviderAccessor review_provider_;   // Guarded by state_mu_.
    ReviewTimeouts review_timeouts_;           // Guarded by state_mu_.
};

} // namespace acecode::agent
