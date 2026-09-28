#include "tui/app/tui_notification_binding.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"
#include "tui/commands/builtin_commands.hpp"
#include "tui/commands/command_registry.hpp"
#include "platform/native_ui/notifications.hpp"
#include "config/config.hpp"
#include "utils/logger.hpp"
namespace acecode::tui {
TuiNotificationBinding::TuiNotificationBinding(const AppConfig& config, TuiState& state,
    IScreenPort& screen, ChatViewport& viewport, SessionManager& session,
    ICommandContextFactory& commands)
    : state_(state), screen_(screen), viewport_(viewport), session_(session), commands_(commands) {
#ifdef _WIN32
    if (config.desktop.notifications.enabled && config.desktop.notifications.on_completion) {
        window_ = desktop::capture_tui_notification_window();
        desktop::NotificationInitOptions options;
        options.app_name = "ACECode TUI";
        options.application_id = "ACECode.ACECode.TUI.1";
        options.activation_window = window_;
        ready_ = desktop::init_notifications(options);
        if (ready_) {
            desktop::set_click_handler([ref = lifetime_.ref(*this)](const desktop::NotifyPayload& payload) {
                const auto session_id = payload.session_id;
                if (session_id.empty()) return;
                ref.with([ref, session_id](TuiNotificationBinding& binding) {
                    binding.screen_.post_task([ref, session_id] {
                        ref.with([&](TuiNotificationBinding& owner) { owner.activate(session_id); });
                    });
                });
            });
        } else {
            LOG_WARN("[tui] native notifications unavailable");
        }
    }
#endif
}
TuiNotificationBinding::~TuiNotificationBinding() { shutdown(); }
void TuiNotificationBinding::shutdown() {
    if (stopped_) return;
    lifetime_.revoke();
#ifdef _WIN32
    desktop::shutdown_notifications();
#endif
    stopped_ = true;
}
void TuiNotificationBinding::activate(const std::string& session_id) {
    if (session_.current_session_id() != session_id) {
        auto context = commands_.make(false);
        resume_session_by_id(context, session_id);
    }
    {
        std::lock_guard<std::mutex> lock(state_.mu);
        viewport_.reset(state_);
        state_.chat_follow_tail = true;
        viewport_.clamp_focus(state_);
    }
    screen_.post_event(ftxui::Event::Custom);
}
}
