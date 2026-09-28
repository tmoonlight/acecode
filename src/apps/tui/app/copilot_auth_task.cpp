#include "tui/app/copilot_auth_task.hpp"
#include "provider/copilot_provider.hpp"
#include "tui/model/user_turn_state.hpp"
using ftxui::Event;
namespace acecode::tui {
CopilotAuthTask::CopilotAuthTask(const AgentLoop::ProviderAccessor& provider_accessor,
    TuiState& s, IScreenPort& scr, std::atomic<bool>& done)
    : state(s), screen(scr), auth_done(done) {
    auto provider_snapshot = provider_accessor();
    copilot = dynamic_cast<CopilotProvider*>(provider_snapshot.get());
    if (copilot && !copilot->is_authenticated()) {
        copilot_model = copilot->model();
        {
            std::lock_guard<std::mutex> lock(state.mu);
            begin_user_turn_locked(state);
            state.conversation.push_back({"system", "Authenticating with GitHub Copilot...", false});
        }
        screen.post_event(Event::Custom);
        worker_ = JoiningThread([this] { authenticate(); });
    } else {
        auth_done = true;
    }
}
void CopilotAuthTask::authenticate() {
    // Try silent auth first (saved token)
    if (copilot->try_silent_auth()) {
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.conversation.push_back({"system", "Authenticated (saved token).", false});
            state.is_waiting = false;
        }
        auth_done = true;
        screen.post_event(Event::Custom);
        return;
    }

    // Need interactive device flow
    auto dc = request_device_code();
    {
        std::lock_guard<std::mutex> lk(state.mu);
        state.conversation.push_back({"system",
            "Open " + dc.verification_uri + " and enter code: " + dc.user_code, false});
    }
    screen.post_event(Event::Custom);

    copilot->run_device_flow([ref = lifetime_.ref(*this)](const std::string& status) {
        ref.with([&](CopilotAuthTask& task) {
            std::lock_guard<std::mutex> lock(task.state.mu);
            task.state.status_line = status;
            task.screen.post_event(Event::Custom);
        });
    });

    if (copilot->is_authenticated()) {
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.conversation.push_back({"system", "GitHub Copilot authenticated!", false});
            state.is_waiting = false;
            state.status_line = "[copilot] model: " + copilot_model;
        }
    } else {
        std::lock_guard<std::mutex> lk(state.mu);
        state.conversation.push_back({"system", "[Error] Authentication failed.", false});
        state.is_waiting = false;
    }
    auth_done = true;
    screen.post_event(Event::Custom);
}
}
