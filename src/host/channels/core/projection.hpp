#pragma once

// 一个绑定的出站投影(design D8):订阅绑定会话的事件,把最终助手文本、会话产生的
// 附件、权限请求、提问发到对应 IM 会话;发送经 RemoteControlHub 的有界 FIFO 串行执行,
// 失败/丢弃计数供设置页显示。
//
// - 平台声明 batch_turn_output(QQ)时,同一回合的助手文本先缓存,回合结束或要发权限
//   请求/提问/文件时合并发出,以节省被动回复额度。
// - 回复引用:用户消息若来自 IM(metadata.channel.reply_context),本回合输出就引用它;
//   Desktop 里输入的消息没有引用(QQ 上表现为主动消息)。
// - 平台支持“正在输入”时,会话忙碌期间保持该状态。

#include "channels/core/store.hpp"
#include "im/transport.hpp"
#include "remote_control/channel_question_bridge.hpp"
#include "remote_control/remote_control_hub.hpp"
#include "session/session_client.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace acecode::channels::core {

// 线程安全地持有当前传输层;开关、改凭据时整体替换。
class TransportSlot {
public:
    std::shared_ptr<im::Transport> get() const;
    void set(std::shared_ptr<im::Transport> transport);

private:
    mutable std::mutex mu_;
    std::shared_ptr<im::Transport> transport_;
};

struct ProjectionDeps {
    SessionClient* sessions = nullptr;
    std::shared_ptr<TransportSlot> transport;
    std::function<std::vector<nlohmann::json>(const std::string&)> pending_permissions;
};

class Projection : public std::enable_shared_from_this<Projection> {
public:
    Projection(BindingRecord record, ProjectionDeps deps);
    ~Projection();
    Projection(const Projection&) = delete;
    Projection& operator=(const Projection&) = delete;

    void start();
    // 退订并排空发送队列;之后不再发送任何东西。
    void stop();

    const BindingRecord& record() const { return record_; }
    // 命令回复等:按 reply_context 引用触发它的消息,走同一个有序队列。
    void say(const std::string& text, const nlohmann::json& reply_context);
    // 一次性取走权限请求;不存在或已关闭返回 false。
    bool take_permission(const std::string& request_id);
    std::vector<std::string> pending_permission_ids() const;
    std::vector<std::string> pending_question_texts();

    // AskUserQuestion 的 /aq 与插话处理需要在锁内驱动问题状态机。
    template <typename Fn>
    auto with_questions(Fn&& fn) -> decltype(fn(std::declval<rc::ChannelQuestionBridge&>())) {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        return fn(questions_);
    }
    void emit_question_action(const rc::ChannelQuestionAction& action);

    rc::RemoteControlStats stats() const { return hub_.stats(); }

private:
    void on_event(const SessionEvent& event);
    void enqueue_text_locked(const std::string& text, const nlohmann::json& reply_context);
    void flush_locked();
    void permission_locked(const nlohmann::json& request);
    void set_typing(bool on);

    BindingRecord record_;
    ProjectionDeps deps_;
    rc::RemoteControlHub hub_;
    rc::ChannelQuestionBridge questions_;
    SessionClient::SubscriptionId subscription_ = 0;
    mutable std::recursive_mutex mu_;
    bool started_ = false;
    bool closed_ = false;
    nlohmann::json reply_context_ = nlohmann::json::object();
    std::string batch_;
    std::map<std::string, nlohmann::json> permissions_;
    std::set<std::string> closed_permissions_;
    std::vector<std::string> closed_order_;
    std::set<std::string> sent_attachments_;
};

} // namespace acecode::channels::core
