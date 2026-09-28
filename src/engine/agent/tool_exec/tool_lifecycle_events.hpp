#pragma once
#include "tool_batch_types.hpp"
#include "tool_stream_progress.hpp"
#include "tool/tool_executor.hpp"
#include "utils/lifetime_token.hpp"

namespace acecode { class EventDispatcher; struct AgentCallbacks; }
namespace acecode::agent {
class ToolLifecycleEvents {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    ToolLifecycleEvents(EventDispatcher& events, SessionManager* session)
        : events_(events), session_manager_(session) {}
    void start(const ToolCall& call, int index, const ToolPreambleTitle& preamble,
        const std::string& preview, const std::string& display, std::int64_t started);
    void finish(const ToolCall& call, int index, const ToolResult& result,
        std::chrono::steady_clock::time_point start, std::int64_t started, ToolCallOutcome& outcome);
    void finish_deferred(const ToolCall& call, int index,
        const ToolCallOutcome& outcome, const ChatMessage& canonical_message);

    // One invocation owns its stream admission token. A retained tool callback
    // becomes inert after this object leaves the joined invocation.
    class Stream {
    public:
        using Update = std::function<void(const std::vector<std::string>&,
            const std::string&, std::size_t, int)>;
        Stream(EventDispatcher& events, AgentCallbacks& callbacks, const ToolCall& call,
            int index, bool emit_tui, Clock clock, std::chrono::steady_clock::time_point start);
        void bind(ToolContext& context);
    private:
        void append(const std::string& chunk);
        EventDispatcher& events_;
        Update callback_;
        std::string name_;
        std::string id_;
        int index_;
        std::string key_;
        Clock clock_;
        std::chrono::steady_clock::time_point start_;
        ToolStreamProgress progress_;
        LifetimeToken lifetime_;
    };
private:
    EventDispatcher& events_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
};
} // namespace acecode::agent
