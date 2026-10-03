#include "agent_mailbox.hpp"

#include <algorithm>
#include <utility>

namespace acecode::agent {

namespace {
// Abort flags are plain atomics without a notifier, so waiters re-check them on
// this cadence (same as ThreadService::wait).
constexpr auto kAbortPollInterval = std::chrono::milliseconds(100);
} // namespace

void AgentMailbox::push(Mail mail) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        mail_.push_back(std::move(mail));
        bump_locked(MailboxActivity::Mailbox);
    }
    cv_.notify_all();
}

bool AgentMailbox::pop_front(Mail& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (mail_.empty()) return false;
    out = std::move(mail_.front());
    mail_.pop_front();
    return true;
}

std::deque<AgentMailbox::Mail> AgentMailbox::take_all() {
    std::deque<Mail> taken;
    std::lock_guard<std::mutex> lock(mu_);
    taken.swap(mail_);
    return taken;
}

std::size_t AgentMailbox::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return mail_.size();
}

bool AgentMailbox::has_trigger_turn() const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::any_of(mail_.begin(), mail_.end(),
                       [](const Mail& mail) { return mail.trigger_turn; });
}

void AgentMailbox::notify_steer() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        bump_locked(MailboxActivity::Steer);
    }
    cv_.notify_all();
}

std::uint64_t AgentMailbox::activity_sequence() const {
    std::lock_guard<std::mutex> lock(mu_);
    return sequence_;
}

MailboxActivity AgentMailbox::wait(std::uint64_t since,
                                   std::chrono::steady_clock::time_point deadline,
                                   const std::atomic<bool>* abort_flag) {
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
        if (sequence_ > since) return last_;
        if (closed_) return MailboxActivity::None;
        if (abort_flag && abort_flag->load()) return MailboxActivity::None;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return MailboxActivity::None;
        cv_.wait_until(lock, std::min(deadline, now + kAbortPollInterval));
    }
}

void AgentMailbox::close() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
    }
    cv_.notify_all();
}

void AgentMailbox::bump_locked(MailboxActivity kind) {
    ++sequence_;
    last_ = kind;
}

} // namespace acecode::agent
