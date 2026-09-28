#pragma once
#include "tui/input/ports.hpp"
#include "utils/lifetime_token.hpp"
namespace acecode { struct TuiState; }
namespace acecode::tui {
class ChatViewport;
class InboundSubmitRegistration {
public:
    InboundSubmitRegistration(TuiState& state, IScreenPort& screen,
        ChatViewport& viewport, ITurnSubmitter& submitter);
    ~InboundSubmitRegistration();
    void stop();
private:
    void submit(const std::string& text);
    TuiState& state_;
    IScreenPort& screen_;
    ChatViewport& viewport_;
    ITurnSubmitter& submitter_;
    bool stopped_ = false;
    LifetimeToken lifetime_;
};
}
