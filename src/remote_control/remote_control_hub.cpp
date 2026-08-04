#include "remote_control_hub.hpp"

#include "outbound_summary.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <exception>
#include <stdexcept>

namespace acecode::rc {

namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool is_blank(const std::string& s) {
    for (unsigned char c : s) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

constexpr const char* kInboundAcknowledgement = "思考中...";
constexpr auto kInboundRouteDrainTimeout = std::chrono::seconds(5);

} // namespace

thread_local std::vector<const RemoteControlHub::InboundDispatchFenceState*>
    RemoteControlHub::active_inbound_dispatch_stack_;

nlohmann::json outbound_message_to_json(const OutboundMessage& msg) {
    nlohmann::json j{
        {"type", msg.type},
        {"session_id", msg.session_id},
        {"timestamp_ms", msg.timestamp_ms},
        {"seq", msg.seq},
    };
    // 可选字段:留空即代表"不适用于本消息类型",不序列化该键 —— channel
    // bridge 侧按键是否存在分支,而不是按空字符串分支。
    if (!msg.text.empty()) j["text"] = msg.text;
    if (!msg.tool_name.empty()) j["tool_name"] = msg.tool_name;
    if (!msg.args_preview.empty()) j["args_preview"] = msg.args_preview;
    if (!msg.in_reply_to.empty()) j["in_reply_to"] = msg.in_reply_to;
    return j;
}

RemoteControlHub::~RemoteControlHub() {
    // 析构不能抛异常。disable 会先 fence 已接受的入站；若析构恰由当前
    // accepted callback 触发，它跳过自等，而 guard 依靠共享 fence state
    // 在 Hub 已结束生命周期后安全收尾。
    try {
        disable();
    } catch (...) {
    }
}

void RemoteControlHub::InboundDispatchGuard::arm(
    std::shared_ptr<InboundDispatchFenceState> state) noexcept {
    state_ = std::move(state);
    active_inbound_dispatch_stack_.push_back(state_.get());
}

RemoteControlHub::InboundDispatchGuard::~InboundDispatchGuard() {
    if (!state_) return;
    assert(!active_inbound_dispatch_stack_.empty());
    assert(active_inbound_dispatch_stack_.back() == state_.get());
    if (!active_inbound_dispatch_stack_.empty()) {
        active_inbound_dispatch_stack_.pop_back();
    }
    std::lock_guard<std::mutex> lk(state_->mu);
    assert(state_->in_flight > 0);
    if (state_->in_flight > 0) --state_->in_flight;
    state_->cv.notify_all();
}

std::size_t RemoteControlHub::active_inbound_dispatch_depth(
    const InboundDispatchFenceState* state) noexcept {
    return static_cast<std::size_t>(std::count(
        active_inbound_dispatch_stack_.begin(),
        active_inbound_dispatch_stack_.end(), state));
}

void RemoteControlHub::wait_for_inbound_dispatches(
    const std::shared_ptr<InboundDispatchFenceState>& state,
    std::size_t allowed_current_thread_depth) {
    std::unique_lock<std::mutex> lk(state->mu);
    state->cv.wait(lk, [&] {
        return state->in_flight <= allowed_current_thread_depth;
    });
}

void RemoteControlHub::set_inbound_submit(InboundSubmit fn) {
    std::lock_guard<std::mutex> lk(mu_);
    inbound_submit_ = std::move(fn);
}

void RemoteControlHub::set_inbound_route(std::string session_id,
                                         InboundSubmit fn) {
    std::lock_guard<std::mutex> lk(mu_);
    if (session_id.empty() || !fn) {
        session_id_.clear();
        inbound_submit_ = {};
        return;
    }
    session_id_ = std::move(session_id);
    inbound_submit_ = std::move(fn);
}

void RemoteControlHub::suspend_inbound_route() {
    std::unique_lock<std::mutex> lk(mu_);
    // A synchronous wait from inside this Hub's own callback can only wait on
    // itself forever. Reject that misuse before changing the route; normal
    // binder commands run on the owned control worker, so production
    // replacement never enters this branch.
    const auto fence = inbound_dispatch_fence_;
    if (active_inbound_dispatch_depth(fence.get()) != 0) {
        throw std::logic_error(
            "cannot suspend an inbound route from its own dispatch callback");
    }
    inbound_submit_ = {};

    auto after_reject = inbound_fence_test_hooks_.after_reject_before_wait;
    lk.unlock();
    std::exception_ptr hook_error;
    if (after_reject) {
        try {
            after_reject();
        } catch (...) {
            hook_error = std::current_exception();
        }
    }
    wait_for_inbound_dispatches(fence);
    if (hook_error) std::rethrow_exception(hook_error);
}

void RemoteControlHub::clear_inbound_route() {
    std::unique_lock<std::mutex> lk(mu_);
    session_id_.clear();
    inbound_submit_ = {};

    // sender 未安装时 worker 不能 dequeue；worker 未运行时也没有进展者。
    // 这两种情况下不能把清路变成无限等待。已在队列中的消息仍遵循原有
    // sender 安装/enable 生命周期。
    if (!worker_.joinable() || !sender_) return;

    // 只等待调用瞬间真实留在 FIFO 中的尾消息。next_seq_ 也会为因
    // drain barrier 满队列而拒绝的新消息递增；以它为目标会等待一个
    // 从未入队、因而永远不会 dequeue 的 seq。
    if (queue_.empty()) return;
    const std::uint64_t barrier_seq = queue_.back().seq;
    if (barrier_seq <= last_dequeued_seq_) return;
    drain_through_seq_ = std::max(drain_through_seq_, barrier_seq);
    cv_.notify_all();
    cv_.wait_for(lk, kInboundRouteDrainTimeout, [this, barrier_seq] {
        return last_dequeued_seq_ >= barrier_seq || !sender_ || stopping_ ||
               !worker_.joinable();
    });
    if (last_dequeued_seq_ < barrier_seq) {
        // 默认 HTTP sender 自带 3 秒超时；额外的 5 秒上限约束 barrier
        // 自身增加的等待。worker join 仍依赖 OutboundSender 的有限返回契约。
        // 超时后关闭优先，尚未被 worker 接管的尾部消息按既有 disable 语义清理。
        if (drain_through_seq_ <= barrier_seq) drain_through_seq_ = 0;
        LOG_WARN("[remote-control] timed out draining outbound queue through seq=" +
                 std::to_string(barrier_seq));
    }
}

void RemoteControlHub::set_inbound_fence_test_hooks(
    InboundFenceTestHooks hooks) {
    std::lock_guard<std::mutex> lk(mu_);
    inbound_fence_test_hooks_ = std::move(hooks);
}

void RemoteControlHub::enable(std::string token,
                              std::string session_id,
                              std::shared_ptr<OutboundSender> sender) {
    // Keep the documented repeat-enable contract: first reject and drain all
    // dispatches accepted by the previous lifecycle, then publish the new one.
    disable();
    std::unique_lock<std::mutex> lk(mu_);
    enabled_ = true;
    stopping_ = false;
    token_ = std::move(token);
    session_id_ = std::move(session_id);
    sender_ = std::move(sender);
    queue_.clear();
    // seq 跨 enable 生命周期保持单调；被本次重建清掉的旧消息应被视作
    // 已越过，避免一次没有新出站的 clear barrier 等待不存在的 seq。
    last_dequeued_seq_ = next_seq_ - 1;
    drain_through_seq_ = 0;
    worker_ = std::thread([this] { worker_loop(); });
}

void RemoteControlHub::disable() {
    std::shared_ptr<InboundDispatchFenceState> fence;
    std::function<void()> after_reject;
    std::size_t current_thread_depth = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // enabled_ participates in the same acceptance lock as handle_inbound,
        // so this is the disable-side linearization point for future inbound.
        enabled_ = false;
        fence = inbound_dispatch_fence_;
        current_thread_depth = active_inbound_dispatch_depth(fence.get());
        after_reject = inbound_fence_test_hooks_.after_reject_before_wait;
    }

    std::exception_ptr hook_error;
    if (after_reject) {
        try {
            after_reject();
        } catch (...) {
            hook_error = std::current_exception();
        }
    }

    // Do not wait for guards on this thread's current call stack, but always
    // wait for every concurrently accepted callback on other threads.
    wait_for_inbound_dispatches(fence, current_thread_depth);

    std::unique_lock<std::mutex> lk(mu_);
    enabled_ = false;
    stop_worker_locked(lk);
    sender_.reset();
    queue_.clear();
    drain_through_seq_ = 0;
    lk.unlock();
    if (hook_error) std::rethrow_exception(hook_error);
}

void RemoteControlHub::stop_worker_locked(std::unique_lock<std::mutex>& lk) {
    if (!worker_.joinable()) return;
    stopping_ = true;
    cv_.notify_all();
    // join 必须放锁外,worker 退出前还要拿一次锁。
    std::thread to_join = std::move(worker_);
    lk.unlock();
    to_join.join();
    lk.lock();
    stopping_ = false;
}

bool RemoteControlHub::enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return enabled_;
}

std::string RemoteControlHub::token() const {
    std::lock_guard<std::mutex> lk(mu_);
    return token_;
}

void RemoteControlHub::set_outbound_sender(std::shared_ptr<OutboundSender> sender) {
    std::lock_guard<std::mutex> lk(mu_);
    sender_ = std::move(sender);
    cv_.notify_all();
}

void RemoteControlHub::set_session_id(std::string session_id) {
    std::lock_guard<std::mutex> lk(mu_);
    session_id_ = std::move(session_id);
}

void RemoteControlHub::set_outbound_result_observer(OutboundResultObserver observer) {
    std::lock_guard<std::mutex> lk(mu_);
    outbound_result_observer_ = std::move(observer);
}

InboundResult RemoteControlHub::handle_inbound(const std::string& text,
                                               const std::string& provided_token) {
    InboundDispatchGuard dispatch_guard;
    InboundSubmit submit;
    std::function<void()> before_dispatch;
    std::string route_session_id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto reject = [this](InboundResult::Code code, std::string msg) {
            ++stats_.inbound_rejected;
            return InboundResult{code, std::move(msg)};
        };
        if (!enabled_) {
            return reject(InboundResult::Code::Disabled, "remote control is off");
        }
        if (provided_token.empty() || provided_token != token_) {
            return reject(InboundResult::Code::BadToken, "invalid token");
        }
        if (text.empty() || is_blank(text)) {
            return reject(InboundResult::Code::BadText, "text must be non-empty");
        }
        if (text.size() > kMaxInboundBytes) {
            return reject(InboundResult::Code::BadText,
                          "text exceeds " + std::to_string(kMaxInboundBytes) + " bytes");
        }
        if (session_id_.empty() || !inbound_submit_) {
            return reject(InboundResult::Code::NoSession, "no session attached");
        }
        route_session_id = session_id_;
        submit = inbound_submit_;
        before_dispatch =
            inbound_fence_test_hooks_.after_accept_before_dispatch;
        auto fence = inbound_dispatch_fence_;
        {
            std::lock_guard<std::mutex> fence_lk(fence->mu);
            ++fence->in_flight;
        }
        dispatch_guard.arm(std::move(fence));
        ++stats_.inbound_accepted;
        // 必须在 submit 前进入同一出站 FIFO:submit 可能立即启动模型或做
        // 协调工作,但确认不能被这些工作拖延。sender 尚未就绪时也保留在
        // 有界队列中,待 set_outbound_sender 后由 hub worker 异步投递。
        enqueue_assistant_text_locked(kInboundAcknowledgement, route_session_id);
    }
    // 锁外调用:submit 内部会拿 TUI state.mu,持 mu_ 调用有死锁风险。
    // dispatch_guard 覆盖测试闸门和 submit 整段；submit 抛异常时析构仍会
    // 递减 accepted-dispatch 计数并唤醒 suspend 等待者。
    if (before_dispatch) before_dispatch();
    submit(text);
    return InboundResult{};
}

void RemoteControlHub::notify_assistant_text(const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_ || !sender_) return;
    if (text.empty() || is_blank(text)) return;
    enqueue_assistant_text_locked(text, session_id_);
}

void RemoteControlHub::enqueue_assistant_text_locked(
    const std::string& text, const std::string& session_id) {
    OutboundMessage msg;
    msg.type = "assistant_message";
    msg.session_id = session_id;
    msg.text = text;
    msg.timestamp_ms = now_ms();
    msg.seq = next_seq_++;
    if (queue_.size() >= kMaxQueue) {
        if (drain_through_seq_ != 0 &&
            queue_.front().seq <= drain_through_seq_) {
            ++stats_.outbound_dropped;
            return;
        }
        queue_.pop_front();
        ++stats_.outbound_dropped;
    }
    queue_.push_back(std::move(msg));
    cv_.notify_all();
}

void RemoteControlHub::notify_tool_call(const std::string& session_id,
                                        const std::string& tool_name,
                                        const nlohmann::json& arguments) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_ || !sender_) return;
    OutboundMessage msg;
    msg.type = "tool_call";
    msg.session_id = session_id;
    msg.tool_name = tool_name;
    msg.args_preview = summarize_tool_args(tool_name, arguments);
    msg.timestamp_ms = now_ms();
    msg.seq = next_seq_++;
    if (queue_.size() >= kMaxQueue) {
        if (drain_through_seq_ != 0 &&
            queue_.front().seq <= drain_through_seq_) {
            ++stats_.outbound_dropped;
            return;
        }
        queue_.pop_front();
        ++stats_.outbound_dropped;
    }
    queue_.push_back(std::move(msg));
    cv_.notify_all();
}

void RemoteControlHub::set_forward_cursor(std::size_t index) {
    std::lock_guard<std::mutex> lk(mu_);
    forward_cursor_ = index;
}

std::size_t RemoteControlHub::forward_cursor() const {
    std::lock_guard<std::mutex> lk(mu_);
    return forward_cursor_;
}

RemoteControlStats RemoteControlHub::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

void RemoteControlHub::worker_loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        cv_.wait(lk, [this] {
            return stopping_ || (!queue_.empty() && sender_ != nullptr);
        });
        if (stopping_) return;
        OutboundMessage msg = std::move(queue_.front());
        queue_.pop_front();
        last_dequeued_seq_ = msg.seq;
        if (drain_through_seq_ != 0 &&
            last_dequeued_seq_ >= drain_through_seq_) {
            drain_through_seq_ = 0;
        }
        cv_.notify_all();
        std::shared_ptr<OutboundSender> sender = sender_;
        lk.unlock();

        std::string error;
        const bool ok = sender->send(msg, &error);

        lk.lock();
        if (ok) {
            ++stats_.outbound_sent;
        } else {
            ++stats_.outbound_failed;
            LOG_WARN("[remote-control] outbound send failed (seq=" +
                     std::to_string(msg.seq) + "): " + error);
        }
        // 结果观察者:拷贝后锁外调用,观察者内部可能回头拿别的锁
        // (daemon 保活判定),持 mu_ 调用有锁序风险。
        OutboundResultObserver observer = outbound_result_observer_;
        if (observer) {
            lk.unlock();
            observer(ok);
            lk.lock();
        }
    }
}

} // namespace acecode::rc
