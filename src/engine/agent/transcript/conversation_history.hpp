#pragma once

#include "llm/llm_provider.hpp"

#include <atomic>
#include <functional>
#include <vector>
#include <unordered_set>

namespace acecode {
class SessionManager;
struct SessionEvent;
struct ThreadRepairOptions;
struct ThreadRepairResult;
}

namespace acecode::agent {

// One writer: the worker, or an idle caller holding its queue gate. The view
// never grants mutation and must not escape the current worker/idle operation.
class ConversationHistory {
public:
    explicit ConversationHistory(const std::atomic<bool>& busy) : busy_(busy) {}
    const std::vector<ChatMessage>& view() const { return messages_; }
    // Assign execution IDs before recording or publishing the new tool batch.
    void prepare_tool_calls(std::vector<ToolCall>& calls);
    void append(ChatMessage message);
    void replace(std::vector<ChatMessage> messages);
    void clear();
    void restore(ChatMessage message, bool worker_or_queue_held);
    void clear_idle(bool worker_or_queue_held);
    void on_worker(const std::function<void(ConversationHistory&)>& operation,
                   bool worker_or_queue_held);
    ThreadRepairResult repair(SessionManager* session, const ThreadRepairOptions& options);
    void observe_transcript(const SessionEvent& event);
    bool retry_blocked() const { return live_tail_blocked_.load(); }

private:
    void remember_tool_call_ids(const ChatMessage& message);
    void warn_unless_idle_access(bool worker_or_queue_held) const;
    const std::atomic<bool>& busy_;
    std::vector<ChatMessage> messages_;
    // Keep reservations across compaction/rewind/clear for this history owner.
    std::unordered_set<std::string> used_tool_call_ids_;
    std::atomic<bool> live_tail_blocked_{false};
};

} // namespace acecode::agent
