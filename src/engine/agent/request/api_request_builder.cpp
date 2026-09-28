#include "agent/agent_loop.hpp"
#include "agent/compaction/compact.hpp"
#include "agent/guards/doom_guard.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/request/request_context.hpp"
#include "computer_use/runtime.hpp"
#include "gitinfo/git_context_collector.hpp"
#include "llm/model_family.hpp"
#include "llm/tool_protocol_names.hpp"
#include "pa/pa_context_budget.hpp"
#include "pa/pa_overflow_rescue.hpp"
#include "permissions/interaction_mode.hpp"
#include "permissions/shell_write_guard.hpp"
#include "prompt/context_usage_breakdown.hpp"
#include "prompt/prompt_environment.hpp"
#include "prompt/system_prompt.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/task_suggestion_store.hpp"
#include "session/thread_goal_store.hpp"
#include "session/thread_repair.hpp"
#include "session/todo_state.hpp"
#include "session/token_tracker.hpp"
#include "session/turn_timing.hpp"
#include "skills/skill_registry.hpp"
#include "skills/skill_usage_store.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "utils/text.hpp"
#include "utils/time.hpp"
#include "utils/uuid.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <thread>

namespace acecode {

using agent::detail::model_facing_provider_messages;
using agent::detail::build_plan_mode_context_prompt;
using agent::detail::append_plan_mode_context_for_api;
using agent::detail::append_todo_context_for_api;
using agent::detail::append_request_context_for_api;
using agent::detail::cached_context_for_api;

std::set<std::string> AgentLoop::dormant_skill_names() const {
    std::set<std::string> out;
    if (!skill_usage_store_ || skill_idle_days_ <= 0 || !skill_registry_) {
        return out;
    }
    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const std::int64_t idle_ms = static_cast<std::int64_t>(skill_idle_days_) *
                                 24LL * 60 * 60 * 1000;
    for (const auto& meta : skill_registry_->list()) {
        if (skill_usage_store_->is_dormant(meta.name, now_ms, idle_ms)) {
            out.insert(meta.name);
        }
    }
    return out;
}

std::vector<ChatMessage> AgentLoop::build_compaction_initial_context() const {
    std::vector<ChatMessage> context;

    SystemPromptWorktreeState worktree_state;
    if (session_manager_) {
        const WorktreeSessionInfo info = session_manager_->active_worktree();
        worktree_state.active = info.active();
        worktree_state.worktree_path = info.worktree_path;
        worktree_state.worktree_branch = info.worktree_branch;
        worktree_state.original_cwd = info.original_cwd;
        worktree_state.inherited = info.inherited;
    }
    const acecode::SystemPromptEnvironment prompt_environment =
        acecode::environment::prompt_environment();
    const SystemPromptSandboxState sandbox_state{sandbox_prompt_description()};
    const SystemPromptModelState model_state = system_prompt_model_state();
    const SystemPromptWorkspaceFolders workspace_folders_state = system_prompt_workspace_folders();
    std::string system_prompt = build_system_prompt(
        tools_, cwd_, skill_registry_, memory_registry_,
        memory_cfg_, project_instructions_cfg_,
        &tool_capability_policy_,
        &worktree_state,
        active_model_can_read_images(),
        &prompt_environment, &sandbox_state, &model_state,
        &workspace_folders_state);
    if (loop_execution_policy_.active &&
        !loop_execution_policy_.system_context.empty()) {
        system_prompt += "\n\n<loop-execution>\n";
        system_prompt += loop_execution_policy_.system_context;
        system_prompt += "\n</loop-execution>";
    }
    if (!system_prompt.empty()) {
        ChatMessage system;
        system.role = "system";
        system.content = std::move(system_prompt);
        context.push_back(std::move(system));
    }

    const std::string git_snapshot =
        git_snapshot_cache_.has_value() ? *git_snapshot_cache_ : std::string{};
    const bool skill_view_available =
        tools_.is_allowed("skill_view", &tool_capability_policy_);
    const bool skills_list_available =
        tools_.is_allowed("skills_list", &tool_capability_policy_);
    const bool spawn_subagent_available =
        tools_.is_allowed("spawn_subagent", &tool_capability_policy_);
    const std::set<std::string> dormant_skills = dormant_skill_names();
    PromptContextBlock skill_context = build_skills_index_context_prompt(
        skill_registry_, context_window_.load(std::memory_order_relaxed),
        skill_view_available, skills_list_available, &dormant_skills);
    if (!skill_context.content.empty()) {
        ChatMessage skill_system;
        skill_system.role = "system";
        skill_system.content = std::move(skill_context.content);
        skill_system.metadata = nlohmann::json{
            {"compact_initial_context", true},
            {"request_local_skill_context", true},
        };
        context.push_back(std::move(skill_system));
    }
    std::string mutable_context = build_session_context_prompt(
        cwd_, memory_registry_, memory_cfg_, project_instructions_cfg_,
        skill_registry_, context_window_.load(std::memory_order_relaxed),
        custom_instructions_cfg_, git_snapshot, expert_, expert_member_id_,
        /*category_bytes=*/nullptr,
        skill_view_available, skills_list_available,
        spawn_subagent_available,
        /*include_skill_index=*/false).content;
    if (!mutable_context.empty()) {
        ChatMessage user;
        user.role = "user";
        user.content = std::move(mutable_context);
        user.metadata = nlohmann::json{{"compact_initial_context", true}};
        context.push_back(std::move(user));
    }
    return context;
}

AgentLoop::ApiRequestBundle AgentLoop::build_api_request_messages(
    bool emergency_profile) {
    ApiRequestBundle bundle;

    // Rebuild the system prompt for each provider call from session-stable
    // inputs. The working directory and date belong here; request-local
    // context below must remain byte-stable while its inputs are unchanged.
    SystemPromptWorktreeState worktree_state;
    if (session_manager_) {
        const WorktreeSessionInfo info = session_manager_->active_worktree();
        worktree_state.active = info.active();
        worktree_state.worktree_path = info.worktree_path;
        worktree_state.worktree_branch = info.worktree_branch;
        worktree_state.original_cwd = info.original_cwd;
        worktree_state.inherited = info.inherited;
    }
    const acecode::SystemPromptEnvironment prompt_environment =
        acecode::environment::prompt_environment();
    const SystemPromptSandboxState sandbox_state{sandbox_prompt_description()};
    const SystemPromptModelState model_state = system_prompt_model_state();
    const SystemPromptWorkspaceFolders workspace_folders_state = system_prompt_workspace_folders();
    std::string system_prompt = build_system_prompt(
        tools_, cwd_, skill_registry_, memory_registry_,
        memory_cfg_, project_instructions_cfg_,
        &tool_capability_policy_,
        &worktree_state,
        active_model_can_read_images(),
        &prompt_environment, &sandbox_state, &model_state,
        &workspace_folders_state);
    if (loop_execution_policy_.active && !loop_execution_policy_.system_context.empty()) {
        system_prompt += "\n\n<loop-execution>\n";
        system_prompt += loop_execution_policy_.system_context;
        system_prompt += "\n</loop-execution>";
    }
    LOG_DEBUG("System prompt length: " + std::to_string(system_prompt.size()));
    auto builtin_tool_defs = tools_.get_model_tool_definitions_by_source(
        ToolSource::Builtin, &tool_capability_policy_);
    auto mcp_tool_defs = tools_.get_model_tool_definitions_by_source(
        ToolSource::Mcp, &tool_capability_policy_);
    if (emergency_profile) {
        // 这里拿到的已是模型侧定义,核心工具名必须经映射取,不能写死 read/write:
        // 「工具重写」关闭时它们叫 file_read / file_write,写死会把核心工具整个滤掉。
        // apply_patch 也算核心:GPT 系模型的编辑工具就是它(下面按模型族再裁)。
        const std::vector<std::string> core_tool_names = {
            model_tool_name_for_native("bash"),
            model_tool_name_for_native("file_read"),
            model_tool_name_for_native("file_write"),
            model_tool_name_for_native("file_edit"),
            model_tool_name_for_native("apply_patch"),
            model_tool_name_for_native("task_complete"),
        };
        const auto is_core_tool = [&core_tool_names](const ToolDef& definition) {
            return std::find(core_tool_names.begin(), core_tool_names.end(),
                             definition.name) != core_tool_names.end();
        };
        builtin_tool_defs.erase(
            std::remove_if(builtin_tool_defs.begin(), builtin_tool_defs.end(),
                           [&](const ToolDef& definition) {
                               return !is_core_tool(definition);
                           }),
            builtin_tool_defs.end());
        mcp_tool_defs.clear();
        LOG_WARN("[thread-repair] using emergency request profile with " +
                 std::to_string(builtin_tool_defs.size()) +
                 " core tool schemas");
        bundle.tool_defs = builtin_tool_defs;
    } else {
        // Preserve the normal model-facing order exactly. The unified helper
        // keeps builtin and MCP tools in registry order, which is also part of
        // prompt-cache stability and expert-switch behavior.
        bundle.tool_defs =
            tools_.get_model_tool_definitions(&tool_capability_policy_);
    }
    // GPT / Codex 系模型只看到 apply_patch,其它模型只看到 file_edit / file_write
    // (openspec add-gpt-apply-patch-adaptation)。三个工具始终注册,这里只裁
    // 模型侧定义表;模型在回合内固定,所以裁完的表逐字节稳定,不打穿 prompt cache。
    filter_tool_definitions_for_model(bundle.tool_defs, model_state.prefers_apply_patch);
    LOG_DEBUG("Registered tools: " + std::to_string(bundle.tool_defs.size()));

    // gitStatus 快照:每会话激活惰性采集一次,cwd 切换或外部失效(Web UI
    // checkout)时重采(openspec add-git-context)。采集失败/非仓库/disabled
    // → 空串不注入。
    if (!emergency_profile && git_snapshot_stale_.exchange(false)) {
        git_snapshot_cache_.reset();
    }
    if (!emergency_profile && !git_snapshot_cache_.has_value()) {
        const bool git_ctx_enabled = !git_context_cfg_ || git_context_cfg_->enabled;
        const int git_timeout_ms = git_context_cfg_
                                       ? git_context_cfg_->timeout_ms
                                       : gitinfo::kDefaultGitTimeoutMs;
        git_snapshot_cache_ =
            git_ctx_enabled
                ? gitinfo::collect_git_status_snapshot(cwd_, git_timeout_ms)
                : std::string();
    }

    // Prepare provider-facing messages with system prompt at front.
    auto api_messages = model_facing_provider_messages(messages_, "provider-request");
    PromptContextCategoryBytes context_category_bytes;
    const bool skill_view_available = !emergency_profile &&
        tools_.is_allowed("skill_view", &tool_capability_policy_);
    const bool skills_list_available = !emergency_profile &&
        tools_.is_allowed("skills_list", &tool_capability_policy_);
    const bool spawn_subagent_available = !emergency_profile &&
        tools_.is_allowed("spawn_subagent", &tool_capability_policy_);
    std::string skill_context;
    std::string session_context;
    if (!emergency_profile) {
        const std::set<std::string> dormant_skills = dormant_skill_names();
        PromptContextBlock skill_context_block = build_skills_index_context_prompt(
            skill_registry_, context_window_.load(std::memory_order_relaxed),
            skill_view_available, skills_list_available, &dormant_skills);
        const bool skill_context_changed =
            skill_context_block.cache_key != skill_context_cache_key_;
        skill_context = cached_context_for_api(
            skill_context_block,
            skill_context_cache_key_, skill_context_cache_content_);
        if (skill_context_changed && !skill_context_block.warning.empty()) {
            LOG_WARN("[skills] " + skill_context_block.warning);
        }
        session_context = cached_context_for_api(
            build_session_context_prompt(
                cwd_, memory_registry_, memory_cfg_, project_instructions_cfg_,
                skill_registry_, context_window_.load(std::memory_order_relaxed),
                custom_instructions_cfg_,
                git_snapshot_cache_.value_or(std::string{}),
                expert_, expert_member_id_,
                &context_category_bytes,
                skill_view_available, skills_list_available,
                spawn_subagent_available,
                /*include_skill_index=*/false),
            session_context_cache_key_, session_context_cache_content_);
    }
    context_category_bytes.skills = skill_context.size();
    std::vector<ChatMessage> mutable_context_messages;
    append_request_context_for_api(mutable_context_messages, session_context);
    std::string swarm_mode_context = emergency_profile
        ? std::string{}
        : build_swarm_mode_context_prompt(
              active_turn_swarm_mode_, spawn_subagent_available);
    append_request_context_for_api(
        mutable_context_messages, swarm_mode_context);
    std::string hook_context = emergency_profile
        ? std::string{} : drain_hook_request_context();
    append_request_context_for_api(mutable_context_messages, hook_context);
    std::string plan_mode_context =
        !emergency_profile && permissions_.mode() == PermissionMode::Plan
            ? build_plan_mode_context_prompt(
                  session_manager_,
                  tools_.is_allowed("AskUserQuestion", &tool_capability_policy_),
                  tools_.is_allowed("ExitPlanMode", &tool_capability_policy_))
            : std::string{};
    append_plan_mode_context_for_api(mutable_context_messages, plan_mode_context);
    std::vector<TodoItem> todo_context_items =
        !emergency_profile && session_manager_
            ? session_manager_->current_todos() : std::vector<TodoItem>{};
    append_todo_context_for_api(mutable_context_messages, todo_context_items);

    ChatMessage skill_system_message;
    if (!skill_context.empty()) {
        skill_system_message.role = "system";
        skill_system_message.content = skill_context;
        skill_system_message.metadata =
            nlohmann::json{{"request_local_skill_context", true}};
    }
    std::vector<ChatMessage> estimated_context_messages =
        mutable_context_messages;
    if (!skill_system_message.content.empty()) {
        estimated_context_messages.insert(
            estimated_context_messages.begin(), skill_system_message);
    }

    bundle.context_usage_estimate = estimate_context_usage_breakdown(
        system_prompt,
        api_messages,
        estimated_context_messages,
        context_category_bytes.project_rules,
        context_category_bytes.skills,
        builtin_tool_defs,
        mcp_tool_defs);

    insert_context_before_last_real_user_or_summary(
        api_messages, std::move(mutable_context_messages));

    ChatMessage sys_msg;
    sys_msg.role = "system";
    sys_msg.content = system_prompt;
    bundle.messages_with_system.push_back(sys_msg);
    if (!skill_system_message.content.empty()) {
        bundle.messages_with_system.push_back(std::move(skill_system_message));
    }
    bundle.messages_with_system.insert(bundle.messages_with_system.end(),
                                       api_messages.begin(), api_messages.end());

    auto prompt_diag = build_prompt_cache_diagnostics(
        system_prompt,
        skill_context + "\n" + session_context + "\n" + swarm_mode_context + "\n" +
            plan_mode_context + "\n" + hook_context + "\n" +
            format_todo_injection(todo_context_items),
        bundle.tool_defs);
    bundle.prompt_diag = {
        {"system", prompt_diag.static_system_prompt_hash},
        {"context", prompt_diag.mutable_context_hash},
        {"tools", prompt_diag.tool_schema_hash},
    };
    LOG_DEBUG("Prompt cache hashes: system=" + prompt_diag.static_system_prompt_hash +
              " context=" + prompt_diag.mutable_context_hash +
              " tools=" + prompt_diag.tool_schema_hash);

    return bundle;
}

} // namespace acecode
