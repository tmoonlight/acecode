#pragma once

// Session-scoped inter-agent mailbox (Codex InputQueue::mailbox_pending_mails).
// Every inbound envelope lands here, never in the steering gate, so aborting or
// erroring a turn cannot drop mail. A running turn drains it at each model
// boundary; after a final answer, queue-only mail waits for the next turn.
// The mutex is a leaf: no method calls out while holding it.

#include "llm/llm_provider.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace acecode::agent {

enum class MailboxActivity { None, Mailbox, Steer };

class AgentMailbox {
public:
    struct Mail {
        UserInput input;
        bool trigger_turn = false;
    };

    void push(Mail mail);
    // Pops the oldest mail; false when empty.
    bool pop_front(Mail& out);
    std::deque<Mail> take_all();
    std::size_t size() const;
    bool has_trigger_turn() const;

    // A user input was steered into the active turn; ends agent_wait early.
    void notify_steer();
    // Monotonic activity counter; agent_wait waits for it to move past a mark.
    std::uint64_t activity_sequence() const;
    // Waits until the counter exceeds `since`, abort is observed, close() is
    // called or the deadline passes. Returns the kind of the newest activity,
    // or None on timeout / abort / close.
    MailboxActivity wait(std::uint64_t since,
                         std::chrono::steady_clock::time_point deadline,
                         const std::atomic<bool>* abort_flag);
    void close();

private:
    void bump_locked(MailboxActivity kind);

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Mail> mail_;
    std::uint64_t sequence_ = 0;
    MailboxActivity last_ = MailboxActivity::None;
    bool closed_ = false;
};

} // namespace acecode::agent
