// AgentLoop 的网状蜂群接口:会话级蜂群上下文发布、跨 agent 投递与 agent_wait。
#include "agent/agent_loop.hpp"
#include "agent/mailbox/agent_mailbox.hpp"
#include "agent/turn/active_turn_gate.hpp"
#include "agent/worker/agent_task_queue.hpp"

#include <utility>

namespace acecode {

void AgentLoop::set_swarm_context(agent::SwarmContext context) {
    std::lock_guard<std::mutex> lock(request_source_mu_);
    request_source_.swarm = std::move(context);
}

agent::SwarmContext AgentLoop::swarm_context() const {
    std::lock_guard<std::mutex> lock(request_source_mu_);
    return request_source_.swarm;
}

void AgentLoop::deliver_inter_agent_message(UserInput envelope, bool trigger_turn) {
    mailbox_->push({std::move(envelope), trigger_turn});
    if (trigger_turn) task_queue_->enqueue_mailbox_wake();
}

AgentLoop::MailboxWaitOutcome AgentLoop::wait_for_mailbox_activity(
    std::chrono::milliseconds timeout, const std::atomic<bool>* abort_flag) {
    // Mark first: activity racing with the pending checks below still moves the
    // counter past the mark, so the wait cannot miss it.
    const auto mark = mailbox_->activity_sequence();
    if (active_turn_gate_->has_pending()) return MailboxWaitOutcome::Steered;
    if (mailbox_->size() > 0) return MailboxWaitOutcome::Mailbox;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    switch (mailbox_->wait(mark, deadline, abort_flag)) {
    case agent::MailboxActivity::Mailbox: return MailboxWaitOutcome::Mailbox;
    case agent::MailboxActivity::Steer: return MailboxWaitOutcome::Steered;
    case agent::MailboxActivity::None: break;
    }
    if (abort_flag && abort_flag->load()) return MailboxWaitOutcome::Aborted;
    return MailboxWaitOutcome::TimedOut;
}

std::size_t AgentLoop::pending_mailbox_count() const {
    return mailbox_->size();
}

bool AgentLoop::has_pending_trigger_mail() const {
    return mailbox_->has_trigger_turn();
}

} // namespace acecode
