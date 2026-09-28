#pragma once
#include "event_dispatcher.hpp"
#include "session_client.hpp"
#include <string>
#include <utility>

namespace acecode {
// Declare after captured state, and destroy without holding listener locks.
// The borrowed dispatcher/client must outlive this resource owner.
class ScopedSubscription {
public:
    ScopedSubscription() = default;
    ScopedSubscription(EventDispatcher& dispatcher, SessionClient::SubscriptionId id)
        : dispatcher_(&dispatcher), id_(id) {}
    ScopedSubscription(SessionClient& client, std::string session_id,
        SessionClient::SubscriptionId id)
        : client_(&client), session_id_(std::move(session_id)), id_(id) {}
    ~ScopedSubscription() { reset(); }
    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;
    ScopedSubscription(ScopedSubscription&& other) noexcept { take(other); }
    ScopedSubscription& operator=(ScopedSubscription&& other) noexcept {
        if (this != &other) { reset(); take(other); }
        return *this;
    }
    void reset() {
        const auto id = std::exchange(id_, 0);
        if (!id) return;
        if (dispatcher_) dispatcher_->unsubscribe_and_wait(id);
        else if (client_) client_->unsubscribe_and_wait(session_id_, id);
    }
    explicit operator bool() const noexcept { return id_ != 0; }
    SessionClient::SubscriptionId id() const noexcept { return id_; }
private:
    void take(ScopedSubscription& other) noexcept {
        dispatcher_ = std::exchange(other.dispatcher_, nullptr);
        client_ = std::exchange(other.client_, nullptr);
        session_id_ = std::move(other.session_id_);
        id_ = std::exchange(other.id_, 0);
    }
    EventDispatcher* dispatcher_ = nullptr; // Nullable borrowed.
    SessionClient* client_ = nullptr; // Nullable borrowed.
    std::string session_id_;
    SessionClient::SubscriptionId id_ = 0;
};
} // namespace acecode
