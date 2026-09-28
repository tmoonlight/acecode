#include "local_session_client.hpp"
#include "environment/data_dir_migration.hpp"

#include "session/ask_user_question_prompter.hpp"
#include "session/session_storage.hpp"
#include "session/system_notice.hpp"
#include "utils/logger.hpp"

namespace acecode {

std::string LocalSessionClient::create_session(const SessionOptions& opts) {
    return registry_.create(opts);
}

bool LocalSessionClient::resume_session(const std::string& id, const SessionOptions& opts) {
    return registry_.resume(id, opts);
}

std::vector<SessionInfo> LocalSessionClient::list_sessions() {
    // v1 简化: 只返回内存活跃的。磁盘历史由 HTTP /api/sessions 单独的
    // SessionStorage::list_sessions 路径合并(后续 Section 9)。
    return registry_.list_active();
}

void LocalSessionClient::destroy_session(const std::string& id) {
    registry_.destroy(id);
}

LocalSessionClient::SubscriptionId
LocalSessionClient::subscribe(const std::string& session_id,
                                EventListener on_event,
                                std::uint64_t since_seq) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) return 0;
    const auto dispatcher_id = entry->loop->events().subscribe(std::move(on_event), since_seq);
    if (!dispatcher_id) return 0;
    std::lock_guard<std::mutex> lock(subscriptions_mu_);
    const auto id = next_subscription_id_++;
    subscriptions_.emplace(id, Subscription{session_id, entry, dispatcher_id});
    return id;
}

void LocalSessionClient::unsubscribe(const std::string& session_id, SubscriptionId sub) {
    remove_subscription(session_id, sub, false);
}
void LocalSessionClient::unsubscribe_and_wait(const std::string& session_id, SubscriptionId sub) {
    remove_subscription(session_id, sub, true);
}
void LocalSessionClient::remove_subscription(
    const std::string& session_id, SubscriptionId sub, bool wait) {
    Subscription subscription;
    {
        std::lock_guard<std::mutex> lock(subscriptions_mu_);
        auto found = subscriptions_.find(sub);
        if (found == subscriptions_.end() || found->second.session_id != session_id) return;
        subscription = std::move(found->second);
        subscriptions_.erase(found);
    }
    // Retain the exact entry even after registry removal; a later resume with
    // the same session id must never receive an old subscription's cleanup.
    const auto entry = subscription.entry.lock();
    if (!entry || !entry->loop) return;
    if (wait) entry->loop->events().unsubscribe_and_wait(subscription.dispatcher_id);
    else entry->loop->events().unsubscribe(subscription.dispatcher_id);
}

bool LocalSessionClient::send_input(const std::string& session_id, const std::string& text) {
    return send_input(session_id, text, std::string{});
}

bool LocalSessionClient::send_input(const std::string& session_id,
                                       const std::string& text,
                                       const std::string& display_text) {
    UserInput input;
    input.text = text;
    input.display_text = display_text;
    return send_input(session_id, input);
}

bool LocalSessionClient::send_input(const std::string& session_id, const UserInput& input) {
    std::shared_lock<std::shared_mutex> migration_lock(environment::data_dir_write_mutex());
    if (environment::data_dir_writes_blocked()) return false;
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) {
        LOG_WARN("[client] send_input on unknown session " + session_id);
        return false;
    }
    // Ordinary submissions lazily refresh only before enqueue. Steering and
    // interruption deliberately remain unhooked because they target the
    // provider already captured by the active turn.
    if (auto reload = registry_.reload_model_profile(session_id, false)) {
        if (!reload->ok) {
            LOG_WARN("[client] model profile reload failed; using current provider");
            entry->loop->emit_system_message(
                "Warning: model profile reload failed; continuing with the current provider.",
                make_system_notice_metadata("model_profile_reload_failed"));
        } else if (!reload->warning.empty()) {
            entry->loop->emit_system_message("Warning: " + reload->warning,
                make_system_notice_metadata("model_profile_warning", {{"text", reload->warning}}));
        }
    }
    registry_.maybe_start_auto_title(session_id, input);
    entry->loop->submit(input);
    return true;
}

bool LocalSessionClient::retry_last_user_message(
    const std::string& session_id,
    const std::string& expected_user_message_id,
    std::string& error) {
    std::shared_lock<std::shared_mutex> migration_lock(environment::data_dir_write_mutex());
    if (environment::data_dir_writes_blocked()) {
        error = "data directory migration is in progress";
        return false;
    }
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) {
        error = "unknown session";
        return false;
    }
    return entry->loop->retry_last_user_message(expected_user_message_id, error);
}

TurnSteerResult LocalSessionClient::steer_input(
    const std::string& session_id,
    const std::string& expected_turn_id,
    const UserInput& input) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) {
        LOG_WARN("[client] steer_input on unknown session " + session_id);
        return {
            TurnSteerStatus::UnknownSession,
            {},
            "unknown session",
        };
    }
    return entry->loop->steer_input(expected_turn_id, input);
}

TurnSteerResult LocalSessionClient::interrupt_turn(
    const std::string& session_id,
    const std::string& expected_turn_id,
    const UserInput& input) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) {
        LOG_WARN("[client] interrupt_turn on unknown session " + session_id);
        return {
            TurnSteerStatus::UnknownSession,
            {},
            "unknown session",
        };
    }
    return entry->loop->interrupt_turn(expected_turn_id, input);
}

TurnSteerResult LocalSessionClient::interject_question(
    const std::string& session_id,
    const std::string& request_id,
    const UserInput& input,
    const std::string& expected_turn_id) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) {
        LOG_WARN("[client] interject_question on unknown session " + session_id);
        return {
            TurnSteerStatus::UnknownSession,
            {},
            "unknown session",
        };
    }
    // 与 steer_input 同款:同回合内的输入不触发模型档案的懒重载,
    // 它面向的是当前回合已经捕获的 provider。
    return entry->loop->interject_question(request_id, input, expected_turn_id);
}

BuiltinCommandResult LocalSessionClient::execute_builtin_command(
    const std::string& session_id,
    const BuiltinCommandRequest& request) {
    return registry_.execute_builtin_command(session_id, request);
}

void LocalSessionClient::respond_permission(const std::string& session_id,
                                               const PermissionDecision& decision) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->prompter) {
        LOG_WARN("[client] respond_permission on unknown session " + session_id);
        return;
    }
    entry->prompter->notify_decision(decision.request_id, decision.choice);
}

QuestionResponseStatus LocalSessionClient::respond_question(
    const std::string& session_id,
    const std::string& request_id,
    const AskUserQuestionResponse& response) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->ask_prompter) {
        LOG_WARN("[client] respond_question on unknown session " + session_id);
        return QuestionResponseStatus::UnknownSession;
    }
    return entry->ask_prompter->notify_response(request_id, response)
               ? QuestionResponseStatus::Accepted
               : QuestionResponseStatus::Closed;
}

std::optional<std::vector<PendingQuestionRequestSnapshot>>
LocalSessionClient::snapshot_pending_questions(const std::string& session_id) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->ask_prompter) return std::nullopt;
    return entry->ask_prompter->snapshot_pending_question_requests();
}

void LocalSessionClient::abort(const std::string& session_id) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) return;
    entry->loop->abort();
}

} // namespace acecode
