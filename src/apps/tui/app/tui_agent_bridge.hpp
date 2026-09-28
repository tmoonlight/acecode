#pragma once
#include "agent/agent_callbacks.hpp"
#include "tui/model/turn_observation.hpp"
#include "utils/lifetime_token.hpp"
#include <utility>
namespace acecode {
struct TuiState; struct AppConfig; class TokenTracker;
}
namespace acecode::tui {
class TuiScreenHost; class IScreenPort; class ChatViewport;
class TuiAgentBridge {
public:
    TuiAgentBridge(TuiState& state, TuiScreenHost& screen, ChatViewport& viewport,
        TokenTracker& tracker, AppConfig& config, TurnObservation& observation);
    AgentCallbacks initial_callbacks();
    void install_progress_callbacks(AgentCallbacks& callbacks);
private:
    template<class... Args>
    auto bind(void (TuiAgentBridge::*method)(Args...)) {
        return [ref = lifetime_.ref(*this), method](Args... args) {
            ref.with([&](TuiAgentBridge& owner) { (owner.*method)(std::forward<Args>(args)...); });
        };
    }
    void on_message(const std::string& role, const std::string& raw_content, bool is_tool);
    void on_transcript_message(const ChatMessage& message);
    void on_busy_changed(bool busy);
    void on_delta(const std::string& token);
    void on_tool_result(const ChatMessage& call_msg, const std::string& tool_name, const ToolResult& result);
    void on_usage(const TokenUsage& usage);
    void on_goal_status(const std::string& status);
    void on_todo_updated(const nlohmann::json& payload);
    void on_thinking_title(const std::string& title);
    void on_transcript_replace(const std::vector<ChatMessage>& /*messages*/, const CompactResult& result);
    void on_stream_retry_reset();
    void on_model_retry(const ProviderErrorInfo& info);
    void on_model_retry_resume();
    void on_turn_finished(const std::string& status);
    void on_tool_progress_start(const std::string& tool_name, const std::string& cmd_preview, const std::string& preamble);
    void on_tool_progress_update(const std::vector<std::string>& tail_snapshot, const std::string& current_partial, size_t total_bytes, int total_lines);
    void on_tool_progress_end();

    TuiState& state;
    TuiScreenHost& screen_host;
    IScreenPort& screen;
    ChatViewport& viewport;
    TokenTracker& token_tracker;
    AppConfig& config;
    TurnObservation& observation;
    LifetimeToken lifetime_;
};
}
