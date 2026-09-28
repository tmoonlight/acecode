#pragma once

#include "tool_session_host.hpp"
#include "tool/tool_executor.hpp"
#include "utils/lifetime_token.hpp"

namespace acecode {
class AbortSignal;
class PermissionManager;
class EventDispatcher;
struct AgentLoopConfig;
}
namespace acecode::agent {
class WorkspaceBoundary;
class SessionExecSecurity;
class PromptContextCache;
class GoalRuntime;

// Bound to one execution scope. Raw dependencies are nullable constructor
// borrows; retained tool callbacks use LifetimeRef and cannot outlive this host.
class ToolContextFactory final : public ToolSessionHost {
public:
    using ProviderAccessor = std::function<std::shared_ptr<LlmProvider>()>;
    ToolContextFactory(WorkspaceBoundary& boundary, SessionExecSecurity& security,
        PromptContextCache& cache, PermissionManager& permissions, GoalRuntime& goal,
        EventDispatcher& events, AbortSignal& abort, SessionManager* session,
        const SkillRegistry* skills, const ToolCapabilityPolicy& policy,
        const AgentLoopConfig& config, ProviderAccessor provider);
    ToolContext for_tool();
    static ToolContext for_user_shell(
        WorkspaceBoundary& boundary, AbortSignal& abort, SessionManager* session);
    std::string cwd() const override;
    std::string write_root() const override;
    std::vector<std::string> writable_workspace_folders() const override;
    bool path_in_workspace_folders(const std::string& path) const override;
    void switch_cwd(const std::string& cwd) override;
    static void switch_cwd(WorkspaceBoundary& boundary, SessionExecSecurity& security,
        PromptContextCache& cache, PermissionManager& permissions,
        SessionManager* session, const std::string& cwd);
private:
    void account_goal_usage();
    void emit_goal_updated(const nlohmann::json& goal_payload);
    void emit_goal_cleared(const std::string& session_id);
    void emit_todo_updated(const nlohmann::json& todo_payload);
    bool goal_unattended_active();
    std::string current_permission_mode();
    ResolvedQuestionPolicy question_policy();
    std::string enter_plan_mode();
    std::string exit_plan_mode();
    void switch_session_cwd(const std::string& new_cwd);
    void track_file_write_before(const std::string& path);
    WorkspaceBoundary& boundary_;
    SessionExecSecurity& security_;
    PromptContextCache& cache_;
    PermissionManager& permissions_;
    GoalRuntime& goal_;
    EventDispatcher& events_;
    AbortSignal& abort_signal_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
    const SkillRegistry* skill_registry_; // Nullable borrowed constructor dependency.
    const ToolCapabilityPolicy& capability_policy_;
    const AgentLoopConfig& loop_cfg_;
    ProviderAccessor provider_accessor_;
    LifetimeToken lifetime_; // Revoke/drain before any dependency can be released.
};

} // namespace acecode::agent
