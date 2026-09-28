#pragma once

#include "llm/llm_provider.hpp"

#include <atomic>
#include <functional>
#include <vector>

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
    void warn_unless_idle_access(bool worker_or_queue_held) const;
    const std::atomic<bool>& busy_;
    std::vector<ChatMessage> messages_;
    std::atomic<bool> live_tail_blocked_{false};
};

} // namespace acecode::agent
