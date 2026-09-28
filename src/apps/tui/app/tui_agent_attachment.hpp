#pragma once
#include "agent/agent_loop.hpp"
#include <cassert>
namespace acecode::tui {
// The design's explicit two-stage assembly exception. Attach once, before any
// event/turn entry; the app owns the agent and stops it before these consumers.
class TuiAgentAttachment {
public:
    void attach(AgentLoop& agent) {
        assert(agent_ == nullptr);
        agent_ = &agent;
    }
protected:
    AgentLoop& agent() const { assert(agent_ != nullptr); return *agent_; }
private:
    AgentLoop* agent_ = nullptr;  // Nullable until attach, then borrowed and fixed.
};
}
