#include "tui/app/inbound_submit_registration.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"
#include "tui/model/user_turn_state.hpp"
#include "remote_control/remote_control_service.hpp"
namespace acecode::tui {
InboundSubmitRegistration::InboundSubmitRegistration(TuiState& state, IScreenPort& screen,
    ChatViewport& viewport, ITurnSubmitter& submitter)
    : state_(state), screen_(screen), viewport_(viewport), submitter_(submitter) {
    rc::remote_control_service().hub().set_inbound_submit(
        [ref = lifetime_.ref(*this)](const std::string& text) {
            ref.with([&](InboundSubmitRegistration& owner) { owner.submit(text); });
        });
}
InboundSubmitRegistration::~InboundSubmitRegistration() { stop(); }
void InboundSubmitRegistration::stop() {
    if (stopped_) return;
    rc::remote_control_service().stop();
    rc::remote_control_service().hub().set_inbound_submit({});
    lifetime_.revoke();
    stopped_ = true;
}
void InboundSubmitRegistration::submit(const std::string& text) {
    std::unique_lock<std::mutex> lock(state_.mu);
    if (state_.is_waiting) {
        state_.pending_queue.push_back(text);
    } else {
        state_.conversation.push_back({"user", text, false});
        state_.chat_follow_tail = true;
        viewport_.clamp_focus(state_);
        begin_user_turn_locked(state_);
        lock.unlock();
        submitter_.before_first_turn();
        lock.lock();
        submitter_.submit_text(text);
    }
    screen_.post_event(ftxui::Event::Custom);
}
}
