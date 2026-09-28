#include "tui/app/tui_app.hpp"
#include "tui/app/tui_services.hpp"
#include "tui/app/tui_screen_host.hpp"
#include "tui/app/tui_submitter.hpp"
#include "tui/app/tui_overlay_gate.hpp"
#include "tui/app/tui_turn_lifecycle.hpp"
#include "tui/app/tui_agent_bridge.hpp"
#include "tui/subagent_host.hpp"
#include "tui/tui_ask_channel.hpp"
#include "tui/resume/session_resume_restore.hpp"
#include "tui/commands/resume_state_sync.hpp"
#include "permissions/default_rules.hpp"
#include "agent/agent_loop.hpp"
#include "session/token_tracker.hpp"
#include "session/permission_prompter.hpp"
#include "session_host/tools/spawn_subagent_tool.hpp"
#include "tool/tool_executor.hpp"
#include "session_host/apply_model_to_session.hpp"
#include "session_host/tools/thread_tools.hpp"
#include "session_host/thread_service.hpp"
#include "tool/mcp_scope.hpp"
#include "tool/mcp_manager.hpp"
#include "skills/skill_registry.hpp"
#include "skills/skill_usage_store.hpp"
#include "memory/memory_registry.hpp"
#include "hooks/hook_manager.hpp"
#include "provider/model_resolver.hpp"
#include "platform/power_inhibitor.hpp"
#include "platform/terminal/terminal_title.hpp"
#include "utils/utf8_path.hpp"
#include <filesystem>
using ftxui::Event;
namespace acecode::tui {
void TuiApp::initialize_agent() {
    permissions_ = std::make_unique<PermissionManager>();
    configure_tui_default_permissions(*permissions_, options_.cli.dangerous_mode, services_->config.default_permission_mode);
    overlay_gate_ = std::make_unique<TuiOverlayGate>(state_, *screen_host_, agent_aborting_);
    turn_lifecycle_ = std::make_unique<TuiTurnLifecycle>(state_, *screen_host_, viewport_, *submitter_,
        session_manager_, services_->config, turn_observation_, auto_title_runner_, notifications_);
    agent_bridge_ = std::make_unique<TuiAgentBridge>(state_, *screen_host_, viewport_, *token_tracker_,
        services_->config, turn_observation_);
    callbacks_ = agent_bridge_->initial_callbacks();
    callbacks_.on_tool_confirm = overlay_gate_->confirm_callback();
    agent_loop_ = std::make_unique<AgentLoop>(provider_accessor_, *services_->tools, callbacks_,
        environment_.working_dir, *permissions_);
    submitter_->attach(*agent_loop_);
    overlay_gate_->attach(*agent_loop_);
    turn_lifecycle_->attach(*agent_loop_);
    auto& config = services_->config;
    auto& tools = *services_->tools;
    auto& skill_registry = *services_->skills;
    auto& memory_registry = *services_->memory;
    auto& mcp_manager = *services_->mcp;
    auto& runtime_memory_cfg = services_->runtime_memory_config;
    auto& hook_manager = *services_->hooks;
    auto& skill_usage_store = services_->skill_usage;
    auto& agent_loop = *agent_loop_;
    auto& working_dir = environment_.working_dir;
    auto& callbacks = callbacks_;
    agent_loop.set_tool_capability_policy(
        mcp_scope_policy(&config, working_dir, std::nullopt, &mcp_manager, &tools));
    // TUI 侧的 AskUserQuestion 传输。接上之后任何工具都能向用户提问
    // (不只是 AskUserQuestion 工具本身),且行为与 daemon 路径同源。
    agent_loop.set_ask_question_channel(bind(&TuiApp::ask_questions));
    agent_loop.set_context_window(config.context_window);
    agent_loop.set_task_suggestion_compact_threshold(
        config.task_suggestion_compact_threshold);
    agent_loop.set_no_model_config_prompt(
        u8"请先配置大模型服务。TUI 可运行 acecode configure 或使用 /model add 添加模型。");
    agent_loop.set_agent_loop_config(config.agent_loop);
    agent_loop.set_sandbox_config(config.sandbox);
    agent_loop.set_hook_manager(&hook_manager);
    agent_loop.set_skill_registry(&skill_registry);
    agent_loop.set_skill_usage_store(skill_usage_store.get());
    agent_loop.set_skill_idle_days(config.skills.idle_days);
    agent_loop.set_memory_registry(&memory_registry);
    agent_loop.set_memory_config(&runtime_memory_cfg);
    agent_loop.set_project_instructions_config(&config.project_instructions);
    agent_loop.set_custom_instructions_config(&config.custom_instructions);
    agent_loop.set_git_context_config(&config.git_context);

    agent_loop.set_callbacks(callbacks);


}
void TuiApp::start_main_session() {
    auto& permissions = *permissions_;
    auto& agent_loop = *agent_loop_;
    auto& session_manager = session_manager_;
    auto& working_dir = environment_.working_dir;
    auto& startup_worktree = environment_.worktree;
    auto& initial_model_profile = services_->initial_model;
    auto& provider_accessor = provider_accessor_;
    {
        auto p = provider_accessor();
        const std::string provider_name = p ? p->name() : std::string{};
        const std::string provider_model = p ? p->model() : std::string{};
        session_manager.start_session(working_dir,
                                      provider_name,
                                      provider_model,
                                      std::string{},
                                      initial_model_profile.name);
        main_session_started_ = true;
        session_manager.set_permission_mode(
            PermissionManager::mode_name(permissions.mode()),
            /*persist_immediately=*/false);
        if (permissions.mode() == PermissionMode::Plan) {
            session_manager.set_pre_plan_permission_mode(
                PermissionManager::mode_name(permissions.pre_plan_mode()),
                /*persist_immediately=*/false);
        }
        // --worktree 启动:worktree 会话状态挂到 SessionManager 并随 meta
        // 持久化,ExitWorktree 工具与退出时的收尾逻辑都以它为准。
        if (startup_worktree.active()) {
            session_manager.set_active_worktree(startup_worktree);
        }
    }
    agent_loop.set_session_manager(&session_manager);
}
void TuiApp::initialize_subagents() {
    auto& config = services_->config;
    auto& tools = *services_->tools;
    auto& skill_registry = *services_->skills;
    auto& memory_registry = *services_->memory;
    auto& mcp_manager = *services_->mcp;
    auto& runtime_memory_cfg = services_->runtime_memory_config;
    auto& hook_manager = *services_->hooks;
    auto& permissions = *permissions_;
    auto& working_dir = environment_.working_dir;
    auto& provider_accessor = provider_accessor_;
    acecode::tui::SubagentHost::Deps subagent_host_deps;
    {
        SessionRegistryDeps rd;
        rd.provider_accessor = provider_accessor;
        rd.tools = &tools;
        rd.cwd = working_dir;
        rd.config = &config;
        rd.mcp_manager = &mcp_manager;
        rd.skill_registry = &skill_registry;
        rd.memory_registry = &memory_registry;
        rd.memory_cfg = &runtime_memory_cfg;
        rd.project_instructions_cfg = &config.project_instructions;
        rd.custom_instructions_cfg = &config.custom_instructions;
        rd.hook_manager = &hook_manager;
        rd.template_permissions = &permissions;
        rd.power_guard = &acecode::process_power_guard();
        subagent_host_deps.registry_deps = std::move(rd);
    }
    subagent_host_deps.parent_session_id = bind(&TuiApp::parent_session_id);
    subagent_host_deps.publish_tasks = bind(&TuiApp::publish_subagent_tasks);
    subagent_host_deps.on_permission_request = bind(&TuiApp::receive_subagent_permission);
    subagent_host_ = std::make_unique<SubagentHost>(std::move(subagent_host_deps));
    auto& subagent_host = *subagent_host_;
    {
        auto subagent_deps = std::make_shared<SubagentToolDeps>();
        subagent_deps->registry = &subagent_host.registry();
        subagent_deps->client = &subagent_host.client();
        subagent_deps->config = &config;
        subagent_deps->fallback_permissions = &permissions;
        subagent_deps->on_spawn = bind(&TuiApp::subagent_spawned);
        tools.register_tool(create_spawn_subagent_tool(subagent_deps));
        tools.register_tool(create_wait_subagent_tool(subagent_deps));
        auto thread_deps = std::make_shared<ThreadToolDeps>();
        thread_deps->service = std::make_shared<ThreadService>(
            ThreadService::Deps{
                &subagent_host.registry(), &subagent_host.client()});
        register_codex_thread_tools(tools, std::move(thread_deps));
    }
}
void TuiApp::resume_startup() {
    auto& state = state_;
    auto& config = services_->config;
    auto& model_binding = services_->model_binding;
    auto& tools = *services_->tools;
    auto& token_tracker = *token_tracker_;
    auto& permissions = *permissions_;
    auto& agent_loop = *agent_loop_;
    auto& session_manager = session_manager_;
    auto& working_dir = environment_.working_dir;
    auto& cwd_override = services_->cwd_model_override;
    const bool resume_latest = options_.cli.resume_latest;
    const std::string& resume_session_id = options_.cli.resume_session_id;
    bool resumed_session_success = false;
    if (resume_latest || !resume_session_id.empty()) {
        std::string target_id = resume_session_id;
        std::string resumed_title;
        if (resume_latest) {
            auto sessions = session_manager.list_sessions();
            if (!sessions.empty()) {
                target_id = sessions.front().id;
                resumed_title = sessions.front().title;
            }
        } else if (!target_id.empty()) {
            // 通过 SessionManager 读取 canonical meta,避免在 TUI 启动路径里
            // 直接拼存储路径。
            auto meta = session_manager.load_session_meta(target_id);
            resumed_title = meta.title;
        }
        if (!target_id.empty()) {
            const bool canonical_exists = session_manager.has_session_file(target_id);
            // 把 session meta 喂给 resolver,让 resume 真正还原 provider+model。
            // resolve_effective_model 会优先用 (meta.provider, meta.model) 从
            // saved_models 找匹配 entry;找不到则构造 ad-hoc entry,name 以
            // "(session:..." 开头,触发我们后面的系统消息提示。
            SessionMeta resumed_meta = session_manager.load_session_meta(target_id);
            std::optional<SessionModelState> resumed_model_state;
            if (auto deleted_state = deleted_model_state_from_meta(config, resumed_meta)) {
                resumed_model_state = deleted_state;
            } else if (!resumed_meta.provider.empty() && !resumed_meta.model.empty()) {
                ModelProfile resumed_entry = resolve_effective_model(
                    config, cwd_override, std::optional<SessionMeta>{resumed_meta});
                ApplyModelDeps deps;
                deps.model_binding = &model_binding;
                deps.sm = &session_manager;
                deps.loop = &agent_loop;
                deps.cfg = &config;
                try {
                    auto result = apply_model_to_session(resumed_entry, deps);
                    config.context_window = result.state.context_window;
                    resumed_model_state = result.state;
                } catch (const std::exception& e) {
                    state.conversation.push_back({"system",
                        std::string("⚠ Resume model switch failed: ") + e.what(), false});
                }
                if (resumed_entry.name.rfind("(session:", 0) == 0) {
                    state.conversation.push_back({"system",
                        "⚠ Resumed with ad-hoc model entry (session recorded " +
                        resumed_meta.provider + "/" + resumed_meta.model +
                        ", not in saved_models). Use /model --default <name> to pick a permanent one.",
                        false});
                }
            }

            auto messages = session_manager.resume_session(target_id);
            const std::string resume_error = session_manager.last_error();
            if (!resume_error.empty()) {
                state.conversation.push_back({"system", resume_error, false});
            } else if (!canonical_exists && session_manager.has_incompatible_session_data(target_id)) {
                state.conversation.push_back({"system",
                    "Session " + target_id + " uses an old PID-suffixed data format that is no longer supported. Delete the old project session data under ~/.acecode/projects and start a new session.", false});
            } else if (!canonical_exists) {
                state.conversation.push_back({"system", "Session " + target_id + " not found.", false});
            } else {
                acecode::append_resumed_session_messages(messages, state, agent_loop, tools);
                state.todos = session_manager.current_todos();
                // worktree 会话恢复:meta 记录的 worktree 还在就把会话 cwd
                // 切回去;目录已被外部删除则清状态,避免 ExitWorktree 之后
                // 操作幽灵路径。
                {
                    const WorktreeSessionInfo resumed_worktree =
                        session_manager.active_worktree();
                    if (resumed_worktree.active()) {
                        std::error_code wt_ec;
                        if (std::filesystem::exists(
                                path_from_utf8(resumed_worktree.worktree_path), wt_ec)) {
                            agent_loop.set_cwd(resumed_worktree.worktree_path);
                            std::filesystem::current_path(
                                path_from_utf8(resumed_worktree.worktree_path), wt_ec);
                            state.conversation.push_back({"system",
                                "Resumed inside worktree " +
                                    resumed_worktree.worktree_path +
                                    (resumed_worktree.worktree_branch.empty()
                                         ? std::string{}
                                         : " (branch " + resumed_worktree.worktree_branch + ")"),
                                false});
                        } else {
                            session_manager.clear_active_worktree();
                            state.conversation.push_back({"system",
                                "Worktree " + resumed_worktree.worktree_path +
                                    " no longer exists; resumed in " + working_dir + ".",
                                false});
                        }
                    }
                }
                const PermissionMode resumed_mode =
                    parse_tui_permission_mode_name(resumed_meta.permission_mode);
                if (resumed_mode == PermissionMode::Plan) {
                    permissions.set_mode(parse_tui_permission_mode_name(
                        resumed_meta.pre_plan_permission_mode.empty()
                            ? std::string{"default"}
                            : resumed_meta.pre_plan_permission_mode));
                    permissions.set_mode(PermissionMode::Plan);
                } else {
                    permissions.set_mode(resumed_mode);
                }
                permissions.clear_session_allows();
                session_manager.set_permission_mode(
                    PermissionManager::mode_name(permissions.mode()),
                    /*persist_immediately=*/false);
                if (permissions.mode() == PermissionMode::Plan) {
                    session_manager.set_pre_plan_permission_mode(
                        PermissionManager::mode_name(permissions.pre_plan_mode()),
                        /*persist_immediately=*/false);
                }
                token_tracker.restore(resumed_meta.last_token_usage,
                                      resumed_meta.session_token_usage);
                sync_tui_resume_runtime_state(state, config, token_tracker,
                                              resumed_model_state);
                state.conversation.push_back({"system",
                    "Resumed session " + target_id + " (" + std::to_string(messages.size()) + " messages)", false});
                resumed_session_success = true;
                agent_loop.publish_current_goal_state();
                agent_loop.maybe_continue_goal();
                if (!resumed_title.empty()) {
                    set_terminal_title(resumed_title);
                    state.current_session_title = resumed_title;
                }
            }
        } else {
            if (session_manager.has_incompatible_session_data()) {
                state.conversation.push_back({"system",
                    "No canonical sessions found. Old PID-suffixed session data in this project is no longer supported; delete the old project session data under ~/.acecode/projects and start a new session.", false});
            } else {
                state.conversation.push_back({"system", "No previous sessions found to resume.", false});
            }
        }
    }
    agent_loop.dispatch_session_start_hook(
        resumed_session_success ? std::string{"resume"} : std::string{"startup"});
    if (resumed_session_success) {
        agent_loop.dispatch_session_title_changed_hook(
            session_manager.current_title(),
            "resume",
            session_manager.current_title_source());
    }
}
void TuiApp::publish_subagent_tasks(std::vector<SubagentTaskSnapshot> tasks) {
    auto& state = state_;
    auto& screen = *screen_host_;
    {
        std::lock_guard<std::mutex> lk(state.mu);
        state.subagent_tasks.clear();
        state.subagent_tasks.reserve(tasks.size());
        for (auto& t : tasks) {
            state.subagent_tasks.push_back(
                {t.id, t.title, t.prompt, t.started});
        }
    }
    screen.post_event(Event::Custom);
}
void TuiApp::receive_subagent_permission(const std::string& session_id, const std::string& task_title, nlohmann::json payload) {
    auto& state = state_;
    auto& screen = *screen_host_;
    TuiState::RemoteConfirmRequest req;
    req.session_id = session_id;
    req.request_id = payload.value("request_id", std::string{});
    req.tool = payload.value("tool", std::string{});
    if (payload.contains("args")) {
        req.args_preview = payload["args"].is_string()
            ? payload["args"].get<std::string>()
            : payload["args"].dump(2);
    }
    req.origin_label = "[subagent] " +
        (task_title.empty() ? session_id : task_title);
    {
        std::lock_guard<std::mutex> lk(state.mu);
        state.remote_confirm_queue.push_back(std::move(req));
    }
    // Custom 事件驱动 CatchEvent 入口的泵,在 overlay 空闲时弹出展示。
    screen.post_event(Event::Custom);
}

nlohmann::json TuiApp::ask_questions(const nlohmann::json& payload,
    const std::atomic<bool>* abort_flag, int timeout, const std::string& origin) {
    return ask_via_tui_overlay(state_, screen_host_->screen(), payload, abort_flag, timeout, origin);
}
std::string TuiApp::parent_session_id() { return session_manager_.current_session_id(); }
void TuiApp::subagent_spawned(const std::string& id, const std::string& prompt) {
    subagent_host_->on_spawned(id, prompt);
}
void TuiApp::respond_permission(const std::string& id, const std::string& request, PermissionResult result) {
    subagent_host_->respond_permission(id, request, permission_result_choice_name(result));
}
}
