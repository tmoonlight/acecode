#pragma once

#include "agent/turn/turn_types.hpp"

#include <cstdint>
#include <string>

namespace acecode {
struct AgentCallbacks;
class EventDispatcher;
class SessionManager;
}

namespace acecode::agent {
class ConversationHistory;
class TurnOutcomeRecord;

class TranscriptWriter {
public:
    TranscriptWriter(ConversationHistory& history, EventDispatcher& events,
                     AgentCallbacks& callbacks, TurnOutcomeRecord& outcome)
        : history_(history), events_(events), callbacks_(callbacks), outcome_(outcome) {}
    void dispatch_message(const std::string& role, const std::string& content, bool is_tool,
                          nlohmann::json metadata, nlohmann::json content_parts);
    void append_turn_timing_record(SessionManager* session, const std::string& user_message_uuid,
                                   std::int64_t started_at_ms, std::int64_t completed_at_ms,
                                   const std::string& status);
    void append_tool_user_prompt(SessionManager* session, const std::string& content,
                                 const std::string& display_text, const std::string& source_tool);
    void emit_system_message(const std::string& content, nlohmann::json metadata);
    void emit_transcript_system_message(SessionManager* session, const std::string& content,
                                        nlohmann::json metadata);
    void inject_shell_turn(const std::string& cmd, const std::string& stdout_text,
                           const std::string& stderr_text, int exit_code);
    void emit_session_summary_updated(SessionManager* session);
    void append_user_turn_message(SessionManager* session, UserTurnInfo& info, bool hidden_goal_context);
    void append_interrupted_turn_context(SessionManager* session, const std::string& turn_id);
    void commit_turn_steering_input(SessionManager* session, UserInput input, const std::string& turn_id);
private:
    ConversationHistory& history_;
    EventDispatcher& events_;
    AgentCallbacks& callbacks_;
    TurnOutcomeRecord& outcome_;
};

} // namespace acecode::agent
