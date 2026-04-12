#pragma once

#include "provider/llm_provider.hpp"
#include "tool/tool_executor.hpp"
#include "permissions.hpp"
#include "utils/path_validator.hpp"
#include "utils/token_tracker.hpp"
#include "session/session_manager.hpp"

#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>

namespace acecode {

// Callbacks for the TUI to observe agent loop events
struct AgentCallbacks {
    // Called when a new message is added to the conversation
    std::function<void(const std::string& role, const std::string& content, bool is_tool)> on_message;

    // Called when the agent starts/stops processing
    std::function<void(bool busy)> on_busy_changed;

    // Called to request user confirmation for a tool call.
    // Returns: Allow, Deny, or AlwaysAllow
    std::function<PermissionResult(const std::string& tool_name, const std::string& arguments)> on_tool_confirm;

    // Called for each streaming delta token (real-time TUI update)
    std::function<void(const std::string& token)> on_delta;

    // Called when token usage data is received from the provider
    std::function<void(const TokenUsage& usage)> on_usage;

    // Called when auto-compact is needed (estimated tokens exceed threshold)
    // Returns true if compaction was performed successfully
    std::function<bool()> on_auto_compact;
};

class AgentLoop {
public:
    AgentLoop(LlmProvider& provider, ToolExecutor& tools, AgentCallbacks callbacks,
              const std::string& cwd, PermissionManager& permissions);

    void set_callbacks(AgentCallbacks cb);

    // Submit a user message and run the agent loop until a final text response.
    // This runs synchronously and should be called from a background thread.
    void submit(const std::string& user_message);

    // Abort the current inference. Safe to call from any thread.
    void abort();

    // Legacy cancel alias
    void cancel() { abort(); }

    // Clear all messages (for /clear command)
    void clear_messages() { messages_.clear(); }

    // Push a message (for session restore)
    void push_message(const ChatMessage& msg) { messages_.push_back(msg); }

    const std::vector<ChatMessage>& messages() const { return messages_; }
    std::vector<ChatMessage>& messages_mut() { return messages_; }

    void set_context_window(int cw) { context_window_ = cw; }

    void set_session_manager(SessionManager* sm) { session_manager_ = sm; }

private:
    LlmProvider& provider_;
    ToolExecutor& tools_;
    AgentCallbacks callbacks_;
    std::vector<ChatMessage> messages_;
    std::atomic<bool> abort_requested_{false};
    std::string cwd_;
    PermissionManager& permissions_;
    PathValidator path_validator_;
    int context_window_ = 128000;
    SessionManager* session_manager_ = nullptr;
};

} // namespace acecode
