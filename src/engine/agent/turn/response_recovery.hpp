#pragma once
#include "agent/turn/turn_types.hpp"

namespace acecode { class SessionManager; }
namespace acecode::agent {
class ConversationHistory;
class TranscriptWriter;
class GoalRuntime;

struct ResponseRecoveryState {
    int empty_response_retries = 0;
    int text_tool_call_corrections = 0;
};
struct ResponseRecoveryResult {
    HandleErrorResult action = HandleErrorResult::Proceed;
    std::string finish_status;
};

// Text-form rejection is checked before blank-response recovery. State is
// consecutive and is reset only when an actual tool batch is produced.
class ResponseRecovery {
public:
    ResponseRecovery(ConversationHistory& history, TranscriptWriter& transcript,
        GoalRuntime& goal, SessionManager* session)
        : history_(history), transcript_(transcript), goal_(goal), session_manager_(session) {}
    ResponseRecoveryResult resolve(const ChatResponse& response,
        ResponseRecoveryState& state, const std::vector<std::string>& model_tool_names);
private:
    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    GoalRuntime& goal_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
};
} // namespace acecode::agent
