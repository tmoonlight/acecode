#pragma once

#include "utils/lifetime_token.hpp"
#include "llm/llm_provider.hpp"
#include "llm/tool_result.hpp"
#include "utils/diff_utils.hpp"
#include "question_policy.hpp"
#include "sandbox/sandbox_policy.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>
#include <optional>
#include <utility>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace acecode {

class SessionManager;
class SkillRegistry;
class ToolExecutor;

// Origin of a registered tool. MCP tools are grouped separately in the system
// prompt so the LLM can distinguish internal versus external capabilities.
enum class ToolSource {
    Builtin = 0,
    Mcp = 1,
};

// Session-local expert policy. Missing members inherit all currently
// registered/global capabilities, while engaged empty sets allow none.
// Built-in tool IDs and MCP server IDs remain separate namespaces.
struct ToolCapabilityPolicy {
    std::optional<std::unordered_set<std::string>> builtin_tools;
    std::optional<std::unordered_set<std::string>> mcp_servers;
    // Built-in tools hidden by the session's swarm mode, mapped to the refusal
    // text returned when the model calls one anyway (session/swarm_mode.hpp).
    std::unordered_map<std::string, std::string> hidden_builtin_tools;
};

// Build and attach the shared fallback used by built-in, MCP, unknown, denied,
// failed, and legacy tool calls that do not provide a domain-specific summary.
// Existing summaries are never overwritten.
ToolSummary build_fallback_tool_summary(
    const std::string& tool_name,
    const std::string& arguments_json);
void ensure_tool_summary(
    const std::string& tool_name,
    const std::string& arguments_json,
    ToolResult& result);

struct ScratchPathResolution {
    bool success = true;
    bool used_alias = false;
    std::string path;
    std::string error;
};

// Runtime context passed into a tool invocation. Optional: if left
// default-constructed, tools behave as if no streaming/abort is available.
// Populated by AgentLoop before each tool call so the tool can push
// interim output to the TUI and react to Esc-driven aborts.
struct ToolContext {
    // Session workspace cwd. Tools that support a default working directory
    // should prefer this over the daemon process cwd when their own arguments
    // omit a cwd/path.
    std::string cwd;
    // 会话写边界根目录(AgentLoop::write_root 的快照);空 = 无边界。
    // spawn_subagent 用它把父会话的边界透传给子会话。工具自身不据此拒写,
    // 拒写统一在 AgentLoop 的路径校验里做。
    std::string write_root;

    // 会话身份(AgentLoop::build_tool_context 填)。Agent Browser 页面按
    // session_id 归属到会话;子代理另带 parent_session_id 供展示归父。
    // workspace_hash 是 projects/<hash> 的目录名,只作附带信息不参与匹配。
    // 三者为空 = 未接线,工具按未绑定会话的旧式请求处理。
    std::string session_id;
    std::string parent_session_id;
    std::string workspace_hash;

    // Called zero or more times with non-empty cleaned chunks (ANSI stripped,
    // UTF-8 boundary safe, carriage-return overwrites resolved). Only bash_tool
    // uses this currently — other tools return their output atomically.
    std::function<void(const std::string& chunk)> stream;
    // Non-owning pointer to AgentLoop::abort_requested_. Tools with long polling
    // loops must check this every iteration and terminate their subprocess /
    // work when it becomes true.
    const std::atomic<bool>* abort_flag = nullptr;
    // Optional file-checkpoint hook used by write tools. Tools call this after
    // validation succeeds and immediately before mutating a file so /rewind can
    // restore the pre-write state.
    std::function<void(const std::string& path)> track_file_write_before;

    // Per-session scratch directory for temporary helper files. AgentLoop
    // injects `.acecode/tmp/session-<id>` under the workspace when a session id
    // is available. Shell tools expose this as ACECODE_TMPDIR.
    std::string scratch_dir;

    // Whether file_path is inside the workspace-managed temporary root. The
    // root is derived from scratch_dir (its parent), so callers never duplicate
    // the `.acecode/tmp` path contract.
    bool is_workspace_scratch_path(const std::string& file_path) const;

    // Recognizes the explicit ACECODE_TMPDIR spellings accepted by shell and
    // structured file tools. This intentionally does not expand arbitrary
    // environment variables.
    static bool references_scratch_path_alias(const std::string& value);

    // Resolves a leading ACECODE_TMPDIR path component to scratch_dir. Invalid
    // placement, unavailable context, and parent traversal fail closed before
    // a file tool reaches filesystem APIs.
    ScratchPathResolution resolve_scratch_path_alias(
        const std::string& file_path) const;

    // Optional async channel for AskUserQuestion. Daemon path injects an impl
    // backed by AskUserQuestionPrompter; TUI path keeps it null and registers
    // the TUI-flavored AskUserQuestion factory which talks to TuiState directly.
    //
    // Wire format (nlohmann::json) — kept loose so this header doesn't pull
    // in session/ headers:
    //   in  questions_payload: array of {id, text, options:[{label, value}], multiSelect}
    //   out: { cancelled: bool,
    //          answers: [ { question_id, selected: [str], custom_text: str } ] }
    // Empty function = AskUserQuestion tool returns the rejected ToolResult.
    std::function<nlohmann::json(const nlohmann::json& questions_payload)> ask_user_questions;

    // Per-session state injected by AgentLoop. Goal tools use this instead of
    // binding to one SessionManager at process-wide tool registration time.
    SessionManager* session_manager = nullptr;

    // Effective per-session Skill registry. Skill tools prefer this over
    // rebuilding a workspace-only registry, which preserves expert-bundled
    // Skill isolation and precedence.
    const SkillRegistry* skill_registry = nullptr;

    // AgentLoop sets this so bash can hand the full output to the
    // tool-result budget layer. Standalone tool callers keep the legacy
    // 100KB inline cap unless they explicitly opt in.
    bool preserve_full_output = false;

    std::function<void()> account_goal_usage;
    std::function<void(const nlohmann::json& goal_payload)> emit_goal_updated;
    std::function<void(const std::string& session_id)> emit_goal_cleared;
    std::function<void(const nlohmann::json& todo_payload)> emit_todo_updated;

    // Goal 运行探针(AgentLoop 注入)。true = 当前会话(或父会话)
    // 有 Active goal 且非 Plan mode。工具权限确认自动放行;
    // AskUserQuestion 正常弹 UI，但固定 30 秒后自动采纳推荐项。
    // 空函数 = 正常交互模式。
    std::function<bool()> goal_unattended_active;

    // AskUserQuestion 应答策略探针(AgentLoop 注入,模式同
    // goal_unattended_active)。返回 resolve_question_policy 的解析结果,
    // permission mode 不参与提问策略解析。空函数 = Ask(独立调用
    // ToolExecutor 的旧行为)。active goal 在工具入口覆盖为 Timeout(30)。
    std::function<ResolvedQuestionPolicy()> question_policy;

    // Plan-mode tools use these callbacks to mutate the active AgentLoop's
    // permission state. They are callbacks rather than direct PermissionManager
    // references so the tool layer stays independent of the TUI/daemon runtime.
    std::function<std::string()> current_permission_mode;
    std::function<std::string()> enter_plan_mode;
    std::function<std::string()> exit_plan_mode;

    // Worktree 工具回调(AgentLoop 注入):把会话工作目录切到 new_cwd,
    // 并以新根重建路径校验器。enter_worktree / exit_worktree 用它在
    // worktree 与原目录之间切换;空函数 = 当前 runtime 不支持切换
    // (独立 ToolExecutor 调用),工具会拒绝执行。
    std::function<void(const std::string& new_cwd)> switch_session_cwd;

    // Runtime access to the active executor. Tools that intentionally change
    // the available tool set can use this to register additional tools for the
    // next model request.
    ToolExecutor* tool_executor = nullptr;

    // Optional session-local expert scope. ToolExecutor checks this again at
    // the execution boundary so replayed/model-produced calls cannot bypass
    // provider schema filtering.
    std::optional<ToolCapabilityPolicy> capability_policy;

    // bash 的沙盒请求(AgentLoop 按 src/sandbox 决策表逐次注入)。空 = 不沙盒,
    // 与独立调用 ToolExecutor 的旧行为一致。bash_tool 只按它行动,不知道模式 /
    // 规则的存在。
    std::optional<sandbox::ExecSandboxRequest> exec_sandbox;

    // 当前 active 模型的身份与视觉能力(AgentLoop 注入)。vision_analyze 用它把
    // "当前模型"从候选视觉模型里剔除:主模型自己带 vision 标签时,子调用很容易
    // 又挑中同一个模型,变成绕一圈用同一个模型看同一张图(实测会话
    // 20260830-024351-9599 白烧了约 5k token)。空串 = 未接线,此时不做剔除以
    // 维持旧行为。
    std::string active_provider_name;
    std::string active_model_id;
    // 与 LlmProvider::supports_vision 同口径,默认 fail-open。
    bool active_model_can_read_images = true;
};

// UI-only metadata contract: a successful structured file change under the
// workspace scratch root must not contribute to the per-turn "modified files"
// summary. The tool row and its diff remain available.
inline constexpr const char* kExcludeFromTurnChangeSummaryMetadata =
    "exclude_from_turn_change_summary";

void mark_workspace_scratch_change(ToolResult& result, const ToolContext& ctx);

// A registered tool implementation. The execute function takes a ToolContext —
// tools that don't need streaming simply ignore it.
struct ToolImpl {
    ToolDef definition;
    std::function<ToolResult(const std::string& arguments_json, const ToolContext& ctx)> execute;
    bool is_read_only = false; // Read-only tools are auto-approved without user confirmation
    ToolSource source = ToolSource::Builtin;
    // Exact owning MCP server ID. Empty for built-ins. This metadata is never
    // inferred from a qualified tool name.
    std::string source_owner;
    // Observation tools can be read-only for permissions yet depend on the
    // order of a stateful desktop session. Keep scheduling separate from it.
    bool requires_serial_execution = false;
    // Model schemas are deferred until this skill has actually loaded in the
    // requesting session. Registration and execution permissions stay separate.
    std::string activation_skill;
};

struct RegisteredToolInfo {
    ToolDef definition;
    bool is_read_only = false;
    ToolSource source = ToolSource::Builtin;
    std::string source_owner;
};

class ToolExecutor {
public:
    ToolExecutor() = default;
    ~ToolExecutor() { lifetime_.revoke(); }
    LifetimeRef<ToolExecutor> lifetime_ref() { return lifetime_.ref(*this); }
    // Register or refresh a tool only when an existing entry has the same
    // source/owner identity. This prevents one MCP server from overwriting a
    // different server (or a built-in) on a qualified-name collision.
    bool register_tool(const ToolImpl& tool);

    // Remove a tool by name. When expected_source_owner is present, removal is
    // allowed only for the exact (name, owner) pair.
    bool unregister_tool(
        const std::string& name,
        std::optional<std::string> expected_source_owner = std::nullopt);

    // Get all tool definitions for inclusion in API requests
    std::vector<ToolDef> get_tool_definitions(
        const ToolCapabilityPolicy* policy = nullptr) const;

    // Get tool definitions filtered by source (built-in vs MCP).
    std::vector<ToolDef> get_tool_definitions_by_source(
        ToolSource source,
        const ToolCapabilityPolicy* policy = nullptr) const;

    // Get tool definitions translated to the public names exposed to models.
    // Internal callers should continue using the native definition methods.
    std::vector<ToolDef> get_model_tool_definitions(
        const ToolCapabilityPolicy* policy = nullptr,
        const std::unordered_set<std::string>& loaded_skills = {}) const;

    std::vector<ToolDef> get_model_tool_definitions_by_source(
        ToolSource source,
        const ToolCapabilityPolicy* policy = nullptr,
        const std::unordered_set<std::string>& loaded_skills = {}) const;

    // Accept an exact registered native name first, then resolve a compatible
    // public alias only when its native handler is registered. 两步都没命中时
    // 再做一次 ASCII 大小写不敏感匹配(原生名 + 当前映射的 public 名),候选
    // **恰好一个**才采用(模型写 Bash → bash);同时有 foo / Foo 时不猜,
    // 原样返回(fail-open,由调用方报 Unknown tool)。
    std::string resolve_model_tool_name_to_native(
        const std::string& model_name) const;

    // Sanitized registration metadata for runtime-backed capability catalogs.
    std::vector<RegisteredToolInfo> get_registered_tools() const;

    // Central policy predicate shared by schema filtering and AgentLoop's
    // pre-permission rejection path.
    bool is_allowed(const std::string& name,
                    const ToolCapabilityPolicy* policy) const;

    // Returns true only for a registered tool excluded by the policy. Unknown
    // tool names remain unknown instead of being mislabeled as policy denials.
    bool is_denied_by_policy(const std::string& name,
                             const ToolCapabilityPolicy* policy) const;
    // Model-facing refusal text for a policy-denied tool (expert policy or a
    // swarm-mode hidden tool).
    static std::string policy_denial_text(const std::string& name,
                                          const ToolCapabilityPolicy* policy);

    // Execute a tool call and return the result. Legacy overload — no streaming,
    // no abort flag. Delegates to the ctx overload with a default context.
    ToolResult execute(const std::string& tool_name, const std::string& arguments_json) const;

    // Execute with a ToolContext. Tools that support streaming will call
    // ctx.stream() as chunks arrive; tools that support cancellation will
    // poll ctx.abort_flag.
    ToolResult execute(const std::string& tool_name, const std::string& arguments_json,
                       const ToolContext& ctx) const;

    // Check if a tool is registered
    bool has_tool(const std::string& name) const;

    // Check if a tool is read-only (auto-approved)
    bool is_read_only(const std::string& name) const;
    bool can_execute_in_parallel(const std::string& name) const;

    // Generate a formatted description of all registered tools for system prompt
    std::string generate_tools_prompt(
        const ToolCapabilityPolicy* policy = nullptr) const;

    // Format a tool result into a ChatMessage suitable for the messages array
    static ChatMessage format_tool_result(const std::string& tool_call_id, const ToolResult& result);

    // Format an assistant message that includes tool calls (from the API response)
    static ChatMessage format_assistant_tool_calls(const ChatResponse& response);

    // Build a compact one-line preview for a tool_call row. For bash takes the
    // command's first 60 chars; for file_read/file_write/file_edit takes the
    // file_path (tail-truncated to 40 chars); other tools return an empty
    // string so the TUI falls back to the legacy `[Tool: X] {JSON}` format.
    static std::string build_tool_call_preview(const std::string& tool_name,
                                               const std::string& arguments_json);

private:
    void filter_deferred_tools(std::vector<ToolDef>& definitions,
                               const std::unordered_set<std::string>& loaded_skills) const;
    std::map<std::string, ToolImpl> tools_;
    mutable std::mutex tools_mu_;
    LifetimeToken lifetime_;
};

} // namespace acecode
