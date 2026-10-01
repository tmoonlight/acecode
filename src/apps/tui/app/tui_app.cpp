#include "tui/app/tui_app.hpp"
#include "tui/app/tui_services.hpp"
#include "session_host/memory_runtime.hpp"
#include "tui/app/tui_screen_host.hpp"
#include "tui/app/tui_submitter.hpp"
#include "tui/app/tui_overlay_gate.hpp"
#include "tui/app/tui_turn_lifecycle.hpp"
#include "tui/app/tui_agent_bridge.hpp"
#include "tui/app/tui_command_context_factory.hpp"
#include "tui/app/model_pool_monitor_subscription.hpp"
#include "tui/app/process_guards.hpp"
#include "tui/app/tui_notification_binding.hpp"
#include "tui/app/inbound_submit_registration.hpp"
#include "tui/app/mcp_status_binding.hpp"
#include "tui/app/update_check_task.hpp"
#include "tui/app/copilot_auth_task.hpp"
#include "tui/app/animation_ticker.hpp"
#include "tui/app/tui_clipboard.hpp"
#include "tui/app/tui_event_router.hpp"
#include "tui/app/full_screen_surfaces.hpp"
#include "tui/render/frame_renderer.hpp"
#include "tui/app/tui_runtime_init.hpp"
#include "tui/model/initial_state.hpp"
#include "tui/subagent_host.hpp"
#include "tui/commands/command_bootstrap.hpp"
#include "tui/slash_command_usage.hpp"
#include "session_host/auto_title_runner.hpp"
#include "session/token_tracker.hpp"
#include "permissions/permissions.hpp"
#include "tool/ask_user_question_tool.hpp"
#include "tool/mcp_manager.hpp"
#include "tool/tool_executor.hpp"
#include "skills/skill_registry.hpp"
#include "platform/terminal/terminal_capability.hpp"
#include "version.hpp"
namespace acecode::tui {
TuiApp::TuiApp(TuiLaunchOptions options) : options_(std::move(options)) {}
TuiApp::~TuiApp() {
    shutdown_.run(*this);
    lifetime_.revoke();
}
int TuiApp::run() { return run_tui_application(*this, shutdown_); }
std::shared_ptr<LlmProvider> TuiApp::provider_snapshot() {
    return services_->model_binding.provider_snapshot();
}
void TuiApp::publish_configuration() {
    auto config = std::make_shared<const AppConfig>(services_->config);
    std::atomic_store(&published_config_, std::move(config));
    if (services_->memory) {
        // 设置中心改了记忆开关 / 记忆摘要:立即对本进程的新请求与调度生效。
        MemoryConfig memory = services_->config.memory;
        if (!memory_runtime_available_) memory.enabled = false;
        services_->memory->update_config(memory);
    }
    if (agent_loop_) {
        auto skills = services_->skills->snapshot();
        agent_loop_->enqueue_control([ref = lifetime_.ref(*this), skills = std::move(skills)] {
            bool applied = false;
            ref.with([&](TuiApp& app) {
                app.agent_loop_->publish_skill_snapshot(skills);
                applied = true;
            });
            return applied;
        });
    }
}
AppConfig TuiApp::config_snapshot() {
    auto snapshot = std::atomic_load(&published_config_);
    return snapshot ? *snapshot : AppConfig{};
}
SessionPromptConfig TuiApp::prompt_config_snapshot() {
    auto config = config_snapshot();
    SessionPromptConfig snapshot;
    snapshot.memory = config.memory;
    if (!memory_runtime_available_) snapshot.memory->enabled = false;
    snapshot.project_instructions = config.project_instructions;
    snapshot.custom_instructions = config.custom_instructions;
    snapshot.git_context = config.git_context;
    return snapshot;
}
bool TuiApp::init_stage(TuiInitStage stage) {
    switch (stage) {
    case TuiInitStage::Environment:
        return initialize_tui_startup_environment(environment_.working_dir, options_.cli,
            environment_.worktree, environment_.worktree_banner);
    case TuiInitStage::Services:
        services_ = std::make_unique<TuiServices>();
        if (!services_->initialize(options_.cli, environment_.working_dir, options_.argv0_dir)) return false;
        memory_runtime_available_ = !services_->config.memory.enabled || services_->memory->service()->enabled();
        publish_configuration();
        provider_accessor_ = bind(&TuiApp::provider_snapshot);
        break;
    case TuiInitStage::InitialState:
        initialize_tui_state_before_screen(state_, services_->config, environment_.working_dir,
            options_.cli.dangerous_mode, *services_->mcp, provider_accessor_());
        state_.skill_usage_store = services_->skill_usage;
        state_.slash_command_usage_counts = read_tui_slash_command_usage();
        if (!environment_.worktree_banner.empty())
            state_.conversation.push_back({"system", environment_.worktree_banner, false});
        version_str_ = "acecode v" ACECODE_VERSION;
        cwd_display_ = environment_.working_dir;
        break;
    case TuiInitStage::Screen: {
        TerminalCapabilities capabilities;
        auto mode = initialize_tui_render_mode(services_->config, options_.cli.force_alt_screen,
            capabilities, conhost_compat_layout_);
        maybe_add_legacy_terminal_hint(state_, services_->config, capabilities, mode, options_.cli.force_alt_screen);
        screen_host_ = std::make_unique<TuiScreenHost>(mode, services_->config.tui);
        screen_host_->activate();
        screen_host_->screen().ForceHandleCtrlC(false);
        screen_host_->screen().SelectionChange([] {});
        break;
    }
    case TuiInitStage::UpdateCheck:
        update_check_ = std::make_unique<UpdateCheckTask>(services_->config, state_, *screen_host_);
        break;
    case TuiInitStage::AskAndMcp:
        services_->tools->register_tool(create_ask_user_question_tool_async(
            services_->config.ask.max_questions, services_->config.ask.max_options));
        mcp_status_ = std::make_unique<McpStatusBinding>(
            *services_->mcp, *services_->tools, state_, *screen_host_);
        submitter_ = std::make_unique<TuiSubmitter>(state_, *screen_host_, services_->config,
            services_->model_binding, session_manager_, *services_->mcp,
            mcp_first_turn_wait_done_, auto_title_runner_, bind(&TuiApp::publish_configuration));
        break;
    case TuiInitStage::CopilotAuth:
        auth_task_ = std::make_unique<CopilotAuthTask>(provider_accessor_, state_, *screen_host_, auth_done_);
        break;
    case TuiInitStage::TokenTracking:
        token_tracker_ = std::make_unique<TokenTracker>();
        state_.token_status = token_tracker_->format_status(services_->config.context_window);
        state_.token_percent = token_tracker_->context_percent(services_->config.context_window);
        state_.cache_hit_percent = token_tracker_->cache_hit_percent();
        break;
    case TuiInitStage::AgentAssembly:
        initialize_agent();
        break;
    case TuiInitStage::ModelPool:
        model_pool_monitor_ = std::make_unique<ModelPoolMonitorSubscription>(
            services_->model_binding, services_->config, *agent_loop_, screen_host_->post_target());
        break;
    case TuiInitStage::MainSession:
        start_main_session();
        break;
    case TuiInitStage::AutoTitle:
        auto_title_runner_ = std::make_unique<AutoTitleRunner>(bind(&TuiApp::config_snapshot),
            session_manager_, *agent_loop_, turn_lifecycle_->title_applied_callback());
        callbacks_.on_turn_finished = turn_lifecycle_->title_finished_callback();
        agent_loop_->set_callbacks(callbacks_);
        start_memory_scheduler();
        break;
    case TuiInitStage::Subagents:
        initialize_subagents();
        break;
    case TuiInitStage::ProcessRegistrations:
        session_finalize_ = std::make_unique<SessionFinalizeRegistration>(session_manager_);
        console_ctrl_ = std::make_unique<ConsoleCtrlHandlerRegistration>();
        break;
    case TuiInitStage::ResumeStartup:
        resume_startup();
        break;
    case TuiInitStage::Commands:
        commands_ = std::make_unique<CommandRegistry>();
        register_slash_commands(*commands_, *services_->skills, services_->config, environment_.working_dir);
        command_contexts_ = std::make_unique<TuiCommandContextFactory>(state_, *agent_loop_,
            services_->model_binding, services_->config, *token_tracker_, *permissions_, *screen_host_,
            session_manager_, *services_->mcp, *services_->tools, *services_->skills, *services_->memory,
            *commands_, environment_.working_dir, *submitter_, subagent_host_.get(), surfaces_,
            bind(&TuiApp::publish_configuration));
        break;
    case TuiInitStage::Notifications:
        notifications_ = std::make_unique<TuiNotificationBinding>(services_->config, state_,
            *screen_host_, viewport_, session_manager_, *command_contexts_);
        break;
    case TuiInitStage::ResumePicker:
        if (options_.cli.resume_picker_on_startup && !options_.cli.direct_resume_requested()) {
            auto context = command_contexts_->make(false);
            commands_->dispatch("/resume", context);
        }
        break;
    case TuiInitStage::FinalAgentCallbacks:
        agent_bridge_->install_progress_callbacks(callbacks_);
        callbacks_.on_busy_changed = turn_lifecycle_->busy_callback();
        agent_loop_->set_callbacks(callbacks_);
        break;
    case TuiInitStage::InboundSubmit:
        inbound_submit_ = std::make_unique<InboundSubmitRegistration>(state_, *screen_host_, viewport_, *submitter_);
        break;
    case TuiInitStage::Animation:
        animation_ = std::make_unique<AnimationTicker>(state_, *screen_host_, viewport_, anim_tick_, conhost_compat_layout_);
        break;
    case TuiInitStage::Components:
        create_components();
        break;
    }
    return true;
}
}
