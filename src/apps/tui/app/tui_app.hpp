#pragma once
#include "tui/app/tui_launch_options.hpp"
#include "tui/app/startup_environment.hpp"
#include "tui/app/tui_init_sequence.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"
#include "tui/render/frame_geometry.hpp"
#include "tui/model/turn_observation.hpp"
#include "agent/agent_callbacks.hpp"
#include "agent/request/session_prompt_config.hpp"
#include "session/session_manager.hpp"
#include "utils/lifetime_token.hpp"
#include <ftxui/component/component_base.hpp>
#include <memory>
#include <type_traits>
#include <utility>
namespace acecode {
class AgentLoop; class TokenTracker; class PermissionManager; class CommandRegistry;
class AutoTitleRunner; class LlmProvider;
}
namespace acecode::tui {
class TuiServices; class TuiScreenHost; class TuiSubmitter; class TuiOverlayGate;
class TuiTurnLifecycle; class TuiAgentBridge; class SubagentHost;
struct SubagentTaskSnapshot;
class TuiCommandContextFactory; class ModelPoolMonitorSubscription;
class SessionFinalizeRegistration; class ConsoleCtrlHandlerRegistration;
class TuiNotificationBinding; class InboundSubmitRegistration; class McpStatusBinding;
class UpdateCheckTask; class CopilotAuthTask; class AnimationTicker;
class TuiEventRouter; class TuiFrameRenderer; class IFullScreenSurfaces; class TuiClipboard;
struct TuiInputContext;

class TuiApp final : private ITuiApplicationLifecycle {
public:
    explicit TuiApp(TuiLaunchOptions options);
    ~TuiApp();
    TuiApp(const TuiApp&) = delete;
    TuiApp& operator=(const TuiApp&) = delete;
    int run();
private:
    bool init_stage(TuiInitStage stage) override;
    void run_event_loop() override;
    void shutdown_step(TuiShutdownStep step) override;
    void initialize_agent();
    void start_main_session();
    void initialize_subagents();
    void resume_startup();
    void create_components();
    std::shared_ptr<LlmProvider> provider_snapshot();
    void publish_configuration();
    // 记忆摘要调度器的宿主回调(tui_app_memory.cpp);经 bind() 走 LifetimeRef。
    void start_memory_scheduler();
    std::vector<std::string> memory_project_dirs();
    bool memory_session_busy(const std::string& session_id);
    void memory_notice(const std::string& session_id, const std::string& text);
    AppConfig config_snapshot();
    SessionPromptConfig prompt_config_snapshot();
    nlohmann::json ask_questions(const nlohmann::json& payload,
        const std::atomic<bool>* abort_flag, int timeout, const std::string& origin);
    std::string parent_session_id();
    AgentLoop* main_agent_loop();
    void publish_subagent_tasks(std::vector<SubagentTaskSnapshot> tasks);
    void receive_subagent_permission(const std::string& session_id,
        const std::string& task_title, nlohmann::json payload);
    void subagent_spawned(const std::string& id, const std::string& prompt);
    void respond_permission(const std::string& id, const std::string& request, PermissionResult result);
    ftxui::Element render_frame();
    template<class R, class... Args>
    auto bind(R (TuiApp::*method)(Args...)) {
        return [ref = lifetime_.ref(*this), method](Args... args) -> R {
            if constexpr (std::is_void_v<R>) {
                ref.with([&](TuiApp& app) { (app.*method)(std::forward<Args>(args)...); });
            } else {
                R result{};
                ref.with([&](TuiApp& app) { result = (app.*method)(std::forward<Args>(args)...); });
                return result;
            }
        };
    }

    // A: service declarations are inert; runtime setup happens only in init.
    TuiLaunchOptions options_;
    StartupEnvironment environment_;
    std::unique_ptr<TuiServices> services_;
    std::shared_ptr<const AppConfig> published_config_;
    bool memory_runtime_available_ = true;
    std::function<std::shared_ptr<LlmProvider>()> provider_accessor_;

    // Shared scalar state outlives every task/component that borrows it.
    AgentCallbacks callbacks_;
    std::atomic<bool> auth_done_{false};
    std::atomic<bool> mcp_first_turn_wait_done_{false};
    std::atomic<int> anim_tick_{0};
    std::string version_str_;
    std::string cwd_display_;
    std::string exit_session_id_;
    bool conhost_compat_layout_ = false;
    bool main_session_started_ = false;

    // B: geometry and state outlive the screen's component tree.
    TuiState state_;
    std::unique_ptr<TuiScreenHost> screen_host_;
    ChatViewport viewport_;
    FrameGeometry geometry_;

    // C: AgentLoop is destroyed before every dependency it borrows.
    std::unique_ptr<TokenTracker> token_tracker_;
    std::unique_ptr<PermissionManager> permissions_;
    SessionManager session_manager_;
    TurnObservation turn_observation_;
    std::atomic<bool> agent_aborting_{false};
    std::unique_ptr<TuiSubmitter> submitter_;
    std::unique_ptr<TuiOverlayGate> overlay_gate_;
    std::unique_ptr<TuiTurnLifecycle> turn_lifecycle_;
    std::unique_ptr<AutoTitleRunner> auto_title_runner_;
    std::unique_ptr<TuiAgentBridge> agent_bridge_;
    std::unique_ptr<AgentLoop> agent_loop_;
    std::unique_ptr<SubagentHost> subagent_host_;
    std::unique_ptr<CommandRegistry> commands_;
    std::unique_ptr<TuiCommandContextFactory> command_contexts_;

    // D: emplaced at the original startup steps, never by constructor order.
    std::unique_ptr<ModelPoolMonitorSubscription> model_pool_monitor_;
    std::unique_ptr<SessionFinalizeRegistration> session_finalize_;
    std::unique_ptr<ConsoleCtrlHandlerRegistration> console_ctrl_;
    std::unique_ptr<TuiNotificationBinding> notifications_;
    std::unique_ptr<InboundSubmitRegistration> inbound_submit_;
    std::unique_ptr<McpStatusBinding> mcp_status_;
    std::unique_ptr<UpdateCheckTask> update_check_;
    std::unique_ptr<CopilotAuthTask> auth_task_;
    std::unique_ptr<AnimationTicker> animation_;

    // E: component callbacks retain LifetimeRef, not references into init locals.
    std::unique_ptr<TuiClipboard> clipboard_;
    std::unique_ptr<TuiInputContext> input_context_;
    std::unique_ptr<TuiEventRouter> event_router_;
    ftxui::Component input_component_;
    std::unique_ptr<TuiFrameRenderer> frame_renderer_;
    std::unique_ptr<IFullScreenSurfaces> surfaces_;
    ftxui::Component root_;

    TuiShutdownSequence shutdown_;
    LifetimeToken lifetime_;
};
}
