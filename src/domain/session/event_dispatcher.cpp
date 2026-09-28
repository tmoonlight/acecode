#include "event_dispatcher.hpp"

#include "utils/logger.hpp"
#include "utils/scope_exit.hpp"

#include <chrono>
#include <utility>

namespace acecode {

namespace {
// Stack, rather than a single id, covers a nested listener that unsubscribes
// its suspended outer listener. Keys are subscription addresses across dispatchers.
class DeliveryFrame {
public:
    explicit DeliveryFrame(const void* subscription)
        : subscription_(subscription), previous_(current_) { current_ = this; }
    ~DeliveryFrame() { current_ = previous_; }
    static bool contains(const void* subscription) {
        for (auto* frame = current_; frame; frame = frame->previous_)
            if (frame->subscription_ == subscription) return true;
        return false;
    }
private:
    const void* subscription_; // Borrowed only for this synchronous call.
    DeliveryFrame* previous_;
    inline static thread_local DeliveryFrame* current_ = nullptr;
};

bool charge_bytes(std::size_t bytes, std::size_t& remaining) {
    if (bytes > remaining) return false;
    remaining -= bytes;
    return true;
}

// Account strings and JSON container nodes without serializing a second full
// payload. Stop traversing as soon as the replay budget is exceeded. This is a
// retained-memory estimate, not an allocator-specific resident-set measurement.
bool charge_json(const nlohmann::json& value, std::size_t& remaining) {
    if (!charge_bytes(sizeof(nlohmann::json), remaining)) return false;
    if (value.is_string()) {
        return charge_bytes(sizeof(std::string), remaining) &&
            charge_bytes(value.get_ref<const std::string&>().size(), remaining);
    }
    if (value.is_binary()) {
        return charge_bytes(sizeof(nlohmann::json::binary_t), remaining) &&
            charge_bytes(value.get_binary().size(), remaining);
    }
    if (value.is_array()) {
        if (!charge_bytes(sizeof(nlohmann::json::array_t), remaining)) return false;
        for (const auto& child : value) {
            if (!charge_json(child, remaining)) return false;
        }
    } else if (value.is_object()) {
        if (!charge_bytes(sizeof(nlohmann::json::object_t), remaining)) return false;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!charge_bytes(sizeof(std::string) + 3 * sizeof(void*), remaining) ||
                !charge_bytes(it.key().size(), remaining) ||
                !charge_json(it.value(), remaining)) return false;
        }
    }
    return true;
}

} // namespace

EventDispatcher::EventDispatcher(std::size_t buffer_capacity,
                                 std::size_t buffer_byte_capacity)
    : buffer_capacity_(buffer_capacity == 0 ? 1 : buffer_capacity)
    , buffer_byte_capacity_(buffer_byte_capacity) {}

std::uint64_t EventDispatcher::emit(SessionEventKind kind, nlohmann::json payload) {
    return emit(kind, std::move(payload), EmitOptions{});
}

std::uint64_t EventDispatcher::emit(SessionEventKind kind, nlohmann::json payload,
                                    EmitOptions options) {
    SessionEvent evt;
    evt.kind         = kind;
    evt.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    evt.payload      = std::move(payload);

    // seq 分配与所有订阅入队在同一把锁下完成:并发 emitter 即使在拿锁顺序上
    // 竞争,每个订阅 pending 的物理顺序也必然与 seq 一致。
    std::vector<std::pair<SubscriptionId, std::shared_ptr<Subscription>>> drain_targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        evt.seq = ++seq_counter_;
        if (options.buffered) {
            push_to_buffer(evt, options.coalesce_key);
        }
        drain_targets.reserve(subscriptions_.size());
        for (auto& [id, sub] : subscriptions_) {
            if (!sub) continue;
            sub->pending.push_back(evt);
            if (!sub->catching_up && !sub->delivering && sub->listener) {
                sub->delivering = true;
                drain_targets.emplace_back(id, sub);
            }
        }
    }
    for (auto& [id, sub] : drain_targets) {
        drain_subscription(id, sub);
    }
    return evt.seq;
}

void EventDispatcher::set_observer(EventObserver observer) {
    SubscriptionId previous = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        previous = observer_subscription_id_;
        observer_subscription_id_ = 0;
    }
    if (previous != 0) unsubscribe(previous);
    if (!observer) return;

    const SubscriptionId installed = subscribe(std::move(observer), 0);
    std::lock_guard<std::mutex> lk(mu_);
    observer_subscription_id_ = installed;
}

void EventDispatcher::drain_subscription(
    SubscriptionId id,
    const std::shared_ptr<Subscription>& sub) {
    while (true) {
        SessionEvent evt;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = subscriptions_.find(id);
            if (it == subscriptions_.end() || it->second != sub || sub->catching_up) {
                sub->delivering = false;
                return;
            }
            if (sub->pending.empty()) {
                sub->delivering = false;
                return;
            }
            evt = std::move(sub->pending.front());
            sub->pending.pop_front();
        }
        deliver_to_subscription(id, sub, evt);
    }
}

bool EventDispatcher::deliver_to_subscription(
    SubscriptionId id, const std::shared_ptr<Subscription>& sub, const SessionEvent& event) {
    EventListener listener;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto current = subscriptions_.find(id);
        if (current == subscriptions_.end() || current->second != sub || !sub->listener)
            return false;
        listener = sub->listener;
        ++sub->in_flight;
    }
    DeliveryFrame delivery(sub.get());
    ScopeExit completed([this, sub, id] {
        std::lock_guard<std::mutex> lock(mu_);
        --sub->in_flight;
        if (sub->in_flight == 0) {
            retired_.erase(id);
            sub->drained.notify_all();
        }
    });
    deliver_to_listener(id, listener, event);
    return true;
}

void EventDispatcher::deliver_to_listener(
    SubscriptionId id, const EventListener& listener, const SessionEvent& evt) const {
    if (!listener) return;
    try {
        listener(evt);
    } catch (...) {
        // A bad frame or callback must not strand delivering/catching_up and
        // leave this subscription accumulating every future event. Keep the
        // subscription alive so transient serialization failures can recover.
        LOG_WARN("[event_dispatcher] listener failed id=" + std::to_string(id));
    }
}

EventDispatcher::SubscriptionId
EventDispatcher::subscribe(EventListener listener, std::uint64_t since_seq) {
    if (!listener) return 0;
    SubscriptionId id = next_sub_id_.fetch_add(1);

    auto sub = std::make_shared<Subscription>();
    sub->listener     = listener;
    sub->catching_up  = true;

    // 第一步(持锁): 快照需要回放的历史事件 + 把订阅注册为 catch-up 态。
    // 注册后,emit() 会把实时事件压进 sub->pending 而非直投。
    std::vector<SessionEvent> to_replay;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (since_seq > 0) {
            for (const auto& buffered : buffer_) {
                if (buffered.event.seq > since_seq) {
                    auto replay = buffered.event;
                    replay.replayed = true;
                    to_replay.push_back(std::move(replay));
                }
            }
        }
        subscriptions_[id] = sub;
    }

    // 第二步(锁外): 按 seq 顺序回放历史事件。期间产生的实时事件都进了 pending。
    if (listener) {
        for (const auto& evt : to_replay)
            if (!deliver_to_subscription(id, sub, evt)) break;
    }

    // 第三步:按序 flush catch-up 期间累积的实时事件;在 pending 清空的同一把锁内
    // 翻转 catching_up=false。之后 emit 会认领 delivering 并从同一个队列继续 drain。
    std::size_t live_buffered = 0;
    while (true) {
        SessionEvent evt;
        bool have_event = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = subscriptions_.find(id);
            if (it == subscriptions_.end()) break;   // 回放途中被 unsubscribe
            if (it->second->pending.empty()) {
                it->second->catching_up = false;
                break;
            }
            evt = std::move(it->second->pending.front());
            it->second->pending.pop_front();
            have_event = true;
        }
        if (have_event) deliver_to_subscription(id, sub, evt);
        if (have_event) ++live_buffered;
    }

    // 只在真正触发了回放 / catch-up 排队时记一条 —— 平时(since=0 且无并发)零噪声。
    // live_buffered>0 即命中了 "实时事件与历史回放并发" 的窗口,正是过去导致
    // 乱序丢帧的场景,现在被有序补发正确吸收;留作回归观测信号。
    if (since_seq > 0 || live_buffered > 0) {
        LOG_DEBUG("[event_dispatcher] subscribe id=" + std::to_string(id) +
                  " since=" + std::to_string(since_seq) +
                  " replayed=" + std::to_string(to_replay.size()) +
                  " live_buffered=" + std::to_string(live_buffered) +
                  " current_seq=" + std::to_string(seq_counter_.load()));
    }

    return id;
}

void EventDispatcher::unsubscribe(SubscriptionId id) {
    unsubscribe_impl(id, false);
}
void EventDispatcher::unsubscribe_and_wait(SubscriptionId id) {
    unsubscribe_impl(id, true);
}
void EventDispatcher::unsubscribe_impl(SubscriptionId id, bool wait) {
    std::shared_ptr<Subscription> removed;
    std::deque<SessionEvent> discarded;
    {
        std::unique_lock<std::mutex> lock(mu_);
        const auto active = subscriptions_.find(id);
        if (active != subscriptions_.end()) {
            removed = std::move(active->second);
            subscriptions_.erase(active);
            discarded.swap(removed->pending);
            if (removed->in_flight != 0) retired_[id] = removed;
        } else {
            const auto retired = retired_.find(id);
            if (retired != retired_.end()) removed = retired->second.lock();
        }
        if (observer_subscription_id_ == id) observer_subscription_id_ = 0;
        if (wait && removed && !DeliveryFrame::contains(removed.get())) {
            removed->drained.wait(lock, [&removed] { return removed->in_flight == 0; });
        }
    }
    // Captured resources and queued payloads are released outside mu_.
}

std::size_t EventDispatcher::listener_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    const std::size_t observer_count =
        observer_subscription_id_ != 0 &&
        subscriptions_.find(observer_subscription_id_) != subscriptions_.end()
            ? 1u
            : 0u;
    return subscriptions_.size() - observer_count;
}

void EventDispatcher::push_to_buffer(const SessionEvent& evt, const std::string& coalesce_key) {
    // 调用方持锁
    if (!coalesce_key.empty()) {
        auto it = coalesced_seq_by_key_.find(coalesce_key);
        if (it != coalesced_seq_by_key_.end()) {
            const std::uint64_t old_seq = it->second;
            for (auto bit = buffer_.begin(); bit != buffer_.end(); ++bit) {
                if (bit->event.seq == old_seq) {
                    erase_buffered_event(bit);
                    break;
                }
            }
        }
    }

    std::size_t remaining = buffer_byte_capacity_;
    if (!charge_bytes(sizeof(BufferedEvent), remaining) ||
        !charge_bytes(coalesce_key.size(), remaining) ||
        (!coalesce_key.empty() &&
         (!charge_bytes(sizeof(std::string) + sizeof(std::uint64_t) +
                            2 * sizeof(void*), remaining) ||
          !charge_bytes(coalesce_key.size(), remaining))) ||
        !charge_json(evt.payload, remaining)) return;
    const std::size_t bytes = buffer_byte_capacity_ - remaining;
    // Evict before copying the new payload, keeping the ring bounded even at
    // insertion time. A single oversized event never displaces useful history.
    while (!buffer_.empty() &&
           (buffer_.size() >= buffer_capacity_ || buffered_bytes_ > remaining)) {
        erase_buffered_event(buffer_.begin());
    }
    buffer_.push_back(BufferedEvent{evt, bytes, coalesce_key});
    buffered_bytes_ += bytes;
    if (!coalesce_key.empty()) coalesced_seq_by_key_[coalesce_key] = evt.seq;
}

void EventDispatcher::erase_buffered_event(std::deque<BufferedEvent>::iterator it) {
    buffered_bytes_ -= it->bytes;
    if (!it->coalesce_key.empty()) coalesced_seq_by_key_.erase(it->coalesce_key);
    buffer_.erase(it);
}

} // namespace acecode
