#include "tui/app/tui_submitter.hpp"
#include "tui/tui_state.hpp"
#include "tool/mcp_startup_coordination.hpp"
#include "session/session_manager.hpp"
#include "utils/logger.hpp"
#include <algorithm>

namespace acecode::tui {
TuiSubmitter::TuiSubmitter(TuiState& state, IScreenPort& screen, AppConfig& config,
    SessionModelBinding& binding, SessionManager& session, McpManager& mcp,
    std::atomic<bool>& first_wait_done,
    const std::function<void(const UserInput&)>& auto_title)
    : state_(state), screen_(screen), config_(config), binding_(binding),
      session_(session), mcp_(mcp), first_wait_done_(first_wait_done), auto_title_(auto_title) {}
std::string TuiSubmitter::cwd() const { return agent().cwd(); }
void TuiSubmitter::cancel() { agent().cancel(); }
void TuiSubmitter::submit_shell(const std::string& command) { agent().submit_shell(command); }
void TuiSubmitter::before_first_turn() {
    const auto result = coordinate_mcp_before_first_turn(
        mcp_, first_wait_done_, std::chrono::milliseconds(1500));
    if (result.should_warn) {
        std::lock_guard<std::mutex> lock(state_.mu);
        state_.conversation.push_back({"system", mcp_first_turn_still_starting_warning(), false});
        screen_.post_event(ftxui::Event::Custom);
    }
}
SessionModelResolvedTarget TuiSubmitter::resolve_model(const std::string& name) {
    auto snapshot = std::make_shared<AppConfig>(config_);
    SessionModelResolvedTarget target;
    target.revision = current_saved_models_revision();
    target.config = snapshot;
    const auto found = std::find_if(snapshot->saved_models.begin(), snapshot->saved_models.end(),
        [&name](const ModelProfile& profile) { return profile.name == name; });
    if (found != snapshot->saved_models.end()) {
        target.profile = *found;
        target.state = session_model_state_from_profile(*snapshot, *found);
    }
    return target;
}
bool TuiSubmitter::apply_transition(const SessionModelState& state, const SessionModelTransition& transition) {
    if (state.context_window > 0) {
        config_.context_window = state.context_window;
        agent().set_context_window(state.context_window);
    }
    if (!transition.provider_published && !transition.selection_changed) return true;
    try { return session_.set_active_provider(state.provider, state.model, state.name); }
    catch (...) { return false; }
}
void TuiSubmitter::submit_input(const UserInput& input) {
    const auto ref = lifetime_.ref(*this);
    const auto reload = binding_.ensure_current(false,
        [] { return current_saved_models_revision(); },
        [ref](const std::string& name) {
            SessionModelResolvedTarget target;
            ref.with([&](TuiSubmitter& owner) { target = owner.resolve_model(name); });
            return target;
        },
        [ref](const SessionModelState& state, const SessionModelTransition& transition) {
            bool applied = false;
            ref.with([&](TuiSubmitter& owner) { applied = owner.apply_transition(state, transition); });
            return applied;
        });
    std::string notice;
    if (!reload.ok) {
        LOG_WARN("[tui] model profile reload failed; using current provider");
        notice = "Warning: model profile reload failed; continuing with the current provider.";
    } else if (!reload.warning.empty()) {
        notice = "Warning: " + reload.warning;
    }
    if (!notice.empty()) {
        screen_.post_task([ref, notice] {
            ref.with([&](TuiSubmitter& owner) {
                std::lock_guard<std::mutex> lock(owner.state_.mu);
                owner.state_.conversation.push_back({"system", notice, false});
                owner.state_.chat_follow_tail = true;
            });
        });
    }
    auto_title_(input);
    agent().submit(input);
}
void TuiSubmitter::submit_text(const std::string& text, const std::string& display_text) {
    UserInput input; input.text = text; input.display_text = display_text;
    submit_input(input);
}
std::function<void(const UserInput&)> TuiSubmitter::callback() {
    return [ref = lifetime_.ref(*this)](const UserInput& input) {
        ref.with([&](TuiSubmitter& owner) { owner.submit_input(input); });
    };
}
}
