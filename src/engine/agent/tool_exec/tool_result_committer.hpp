#pragma once
#include "agent/agent_runtime_env.hpp"
#include "tool_batch_types.hpp"

namespace acecode { class SessionManager; }
namespace acecode::agent {
class ConversationHistory;
class TranscriptWriter;
class ToolLifecycleEvents;
class ToolResultCommitter {
public:
    ToolResultCommitter(ConversationHistory& history, TranscriptWriter& transcript,
        ToolLifecycleEvents& events, SessionManager* session, AgentRuntimeEnv environment = {})
        : environment_(std::move(environment)), history_(history), transcript_(transcript),
          lifecycle_events_(events), session_manager_(session) {}
    ToolBatchOutcome commit(std::vector<ToolCallSlot>& slots);
private:
    AgentRuntimeEnv environment_;
    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    ToolLifecycleEvents& lifecycle_events_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
};
} // namespace acecode::agent
