#pragma once

// 通道核心(host/channels/core)单测共用的假对象:
//   - FakeSessions:线程安全的内存版 SessionClient,可设置会话列表、忙碌、删除;
//     send_input / respond_permission 等会像真实会话一样回推事件。
//   - FakeTransport:可编排的假传输层,记录所有发送,可注入入站消息与状态变化。
// 出站投影经 RemoteControlHub 的后台线程发送,断言前用 wait_until 等待。

#include "channels/core/conversations.hpp"
#include "im/transport.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/session_client.hpp"
#include "test_support/channels/test_support.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace acecode::channels::core::test {

inline bool wait_until(const std::function<bool()>& ready,
                       std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return ready();
}

class FakeSessions : public SessionClient {
public:
    struct Interjection {
        std::string session_id;
        std::string request_id;
        UserInput input;
    };

    std::string create_session(const SessionOptions& options) override {
        std::lock_guard<std::mutex> lock(mu);
        const auto id = "ch-" + std::to_string(++counter);
        created.emplace_back(id, options);
        SessionInfo info;
        info.id = id;
        info.no_workspace = options.no_workspace;
        info.workspace_hash = options.workspace_hash;
        info.cwd = options.no_workspace ? "C:/fake-no-workspace/" + id : options.cwd;
        info.title = "会话 " + id;
        info.active = true;
        sessions[id] = info;
        return id;
    }
    bool resume_session(const std::string& id, const SessionOptions&) override {
        std::lock_guard<std::mutex> lock(mu);
        resumed.push_back(id);
        return sessions.count(id) > 0 && !deleted.count(id);
    }
    std::vector<SessionInfo> list_sessions() override {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<SessionInfo> out;
        for (const auto& [id, info] : sessions)
            if (!deleted.count(id)) out.push_back(info);
        return out;
    }
    void destroy_session(const std::string&) override {}
    SubscriptionId subscribe(const std::string& id, EventListener listener, std::uint64_t) override {
        std::lock_guard<std::mutex> lock(mu);
        const auto sub = next_sub++;
        listeners[id][sub] = std::move(listener);
        return sub;
    }
    void unsubscribe(const std::string& id, SubscriptionId sub) override {
        std::lock_guard<std::mutex> lock(mu);
        listeners[id].erase(sub);
    }
    bool send_input(const std::string& id, const std::string& text) override {
        UserInput input;
        input.text = text;
        return send_input(id, input);
    }
    bool send_input(const std::string& id, const UserInput& input) override {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!accept_input || deleted.count(id) || !sessions.count(id)) return false;
            inputs.emplace_back(id, input);
        }
        emit(id, SessionEventKind::Message,
             {{"role", "user"}, {"content", input.text}, {"metadata", input.metadata}});
        return true;
    }
    TurnSteerResult interject_question(const std::string& id, const std::string& request_id, const UserInput& input,
                                       const std::string&) override {
        bool ok;
        {
            std::lock_guard<std::mutex> lock(mu);
            interjections.push_back({id, request_id, input});
            ok = interject_ok;
        }
        if (!ok) return {TurnSteerStatus::NoPendingQuestion, {}, "no pending question"};
        emit(id, SessionEventKind::QuestionClosed, {{"request_id", request_id}, {"reason", "interjected"}});
        return {TurnSteerStatus::Accepted, "turn-1", {}};
    }
    BuiltinCommandResult execute_builtin_command(const std::string&, const BuiltinCommandRequest&) override {
        return {};
    }
    void respond_permission(const std::string& id, const PermissionDecision& decision) override {
        {
            std::lock_guard<std::mutex> lock(mu);
            decisions.emplace_back(id, decision);
        }
        emit(id, SessionEventKind::PermissionClosed,
             {{"request_id", decision.request_id}, {"choice", to_string(decision.choice)}});
    }
    QuestionResponseStatus respond_question(const std::string& id, const std::string& request,
                                            const AskUserQuestionResponse&) override {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!answered.insert(request).second) return QuestionResponseStatus::Closed;
        }
        emit(id, SessionEventKind::QuestionClosed, {{"request_id", request}, {"reason", "answered"}});
        return QuestionResponseStatus::Accepted;
    }
    std::optional<std::vector<PendingQuestionRequestSnapshot>> snapshot_pending_questions(
        const std::string&) override {
        return std::vector<PendingQuestionRequestSnapshot>{};
    }
    void abort(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mu);
        aborts.push_back(id);
    }

    // ---- 测试操作 ----
    void emit(const std::string& id, SessionEventKind kind, const nlohmann::json& payload) {
        std::map<SubscriptionId, EventListener> copy;
        std::uint64_t seq;
        {
            std::lock_guard<std::mutex> lock(mu);
            copy = listeners[id];
            seq = ++next_seq;
        }
        for (const auto& item : copy) item.second(SessionEvent{kind, seq, 0, payload});
    }
    void assistant(const std::string& id, const std::string& text) {
        emit(id, SessionEventKind::Message, {{"role", "assistant"}, {"content", text}});
    }
    void add_session(const std::string& id, const std::string& cwd, const std::string& workspace_hash,
                     bool no_workspace, const std::string& title) {
        std::lock_guard<std::mutex> lock(mu);
        SessionInfo info;
        info.id = id;
        info.cwd = cwd;
        info.workspace_hash = workspace_hash;
        info.no_workspace = no_workspace;
        info.title = title;
        sessions[id] = info;
    }
    void set_busy(const std::string& id, bool busy) {
        std::lock_guard<std::mutex> lock(mu);
        sessions[id].busy = busy;
    }
    void remove(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        deleted.insert(id);
    }
    std::size_t listener_count(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        return listeners[id].size();
    }
    std::vector<std::pair<std::string, UserInput>> input_log() {
        std::lock_guard<std::mutex> lock(mu);
        return inputs;
    }
    std::vector<std::pair<std::string, SessionOptions>> created_log() {
        std::lock_guard<std::mutex> lock(mu);
        return created;
    }
    std::vector<std::string> abort_log() {
        std::lock_guard<std::mutex> lock(mu);
        return aborts;
    }

    // 会话目录:与 build_rc_session_catalog 同形,按 id 排序。
    std::vector<rc::RcSessionTarget> catalog() {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<rc::RcSessionTarget> out;
        for (const auto& [id, info] : sessions) {
            if (deleted.count(id)) continue;
            rc::RcSessionTarget target;
            target.session_id = id;
            target.cwd = info.cwd;
            target.workspace_hash = info.workspace_hash;
            target.no_workspace = info.no_workspace;
            target.title = info.title;
            out.push_back(target);
        }
        return out;
    }

    std::mutex mu;
    int counter = 0;
    bool accept_input = true;
    bool interject_ok = true;
    std::uint64_t next_sub = 1, next_seq = 0;
    std::map<std::string, SessionInfo> sessions;
    std::set<std::string> deleted, answered;
    std::vector<std::pair<std::string, SessionOptions>> created;
    std::vector<std::string> resumed, aborts;
    std::vector<std::pair<std::string, UserInput>> inputs;
    std::vector<Interjection> interjections;
    std::vector<std::pair<std::string, PermissionDecision>> decisions;
    std::map<std::string, std::map<SubscriptionId, EventListener>> listeners;
};

class FakeTransport : public im::Transport {
public:
    struct Sent {
        im::Address to;
        std::string text;
        nlohmann::json context;
        std::string file;  // send_file 时为文件名
    };

    explicit FakeTransport(std::string name = "telegram", im::Capabilities caps = {})
        : name_(std::move(name)), caps_(caps) {}

    std::string platform() const override { return name_; }
    im::Capabilities capabilities() const override { return caps_; }
    void start(im::TransportCallbacks callbacks) override {
        im::TransportStatus snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            callbacks_ = std::move(callbacks);
            ++starts;
            status_.state = im::LinkState::Connected;
            status_.account = account;
            status_.extra = extra;
            snapshot = status_;
        }
        notify(snapshot);
    }
    void stop() override {
        std::lock_guard<std::mutex> lock(mu_);
        callbacks_ = {};
        ++stops;
        status_.state = im::LinkState::Stopped;
    }
    im::TransportStatus status() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return status_;
    }
    im::SendResult send_text(const im::Address& to, const std::string& text, const nlohmann::json& context) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (fail_sends > 0) {
            --fail_sends;
            return {im::SendOutcome::Failed, "平台拒绝"};
        }
        sent_.push_back({to, text, context, {}});
        return {im::SendOutcome::Sent, {}};
    }
    im::SendResult send_file(const im::Address& to, const std::filesystem::path& path, const std::string& name,
                             const std::string&, const nlohmann::json& context) override {
        std::lock_guard<std::mutex> lock(mu_);
        sent_.push_back({to, path_to_utf8(path), context, name});
        return {im::SendOutcome::Sent, {}};
    }
    void set_typing(const im::Address&, bool on) override {
        std::lock_guard<std::mutex> lock(mu_);
        typing.push_back(on);
    }
    bool download(const im::Attachment&, const std::filesystem::path& dest, std::string* error) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!download_ok) {
            if (error) *error = "下载失败";
            return false;
        }
        std::ofstream out(dest, std::ios::binary);
        out << download_bytes;
        return true;
    }
    nlohmann::json action(const std::string& name, const nlohmann::json&) override {
        std::lock_guard<std::mutex> lock(mu_);
        actions.push_back(name);
        return {{"ok", true}};
    }

    // ---- 测试操作 ----
    void inject(const im::Inbound& inbound) {
        std::function<void(im::Inbound)> on_inbound;
        {
            std::lock_guard<std::mutex> lock(mu_);
            on_inbound = callbacks_.on_inbound;
        }
        if (on_inbound) on_inbound(inbound);
    }
    void report(im::LinkState state, const std::string& detail) {
        im::TransportStatus snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            status_.state = state;
            status_.detail = detail;
            status_.retry_stopped = state == im::LinkState::Failed;  // 假定 Failed 都是致命错误
            snapshot = status_;
        }
        notify(snapshot);
    }
    std::vector<Sent> sent() const {
        std::lock_guard<std::mutex> lock(mu_);
        return sent_;
    }
    std::vector<std::string> texts() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::string> out;
        for (const auto& item : sent_)
            if (item.file.empty()) out.push_back(item.text);
        return out;
    }
    // 是否有一条发送包含 needle。
    bool said(const std::string& needle) const {
        for (const auto& text : texts())
            if (text.find(needle) != std::string::npos) return true;
        return false;
    }
    bool wait_said(const std::string& needle) const {
        return wait_until([&] { return said(needle); });
    }
    std::size_t sent_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return sent_.size();
    }

    std::string account = "100";
    nlohmann::json extra = nlohmann::json::object();
    int fail_sends = 0;
    bool download_ok = true;
    std::string download_bytes = "image-bytes";
    int starts = 0, stops = 0;
    std::vector<bool> typing;
    std::vector<std::string> actions;

private:
    void notify(const im::TransportStatus& status) {
        std::function<void(const im::TransportStatus&)> on_status;
        {
            std::lock_guard<std::mutex> lock(mu_);
            on_status = callbacks_.on_status;
        }
        if (on_status) on_status(status);
    }

    std::string name_;
    im::Capabilities caps_;
    mutable std::mutex mu_;
    im::TransportCallbacks callbacks_;
    im::TransportStatus status_;
    std::vector<Sent> sent_;
};

inline im::Address private_address(const std::string& user, const std::string& platform = "telegram",
                                   const std::string& account = "100") {
    im::Address address;
    address.platform = platform;
    address.account = account;
    address.kind = im::ChatKind::Private;
    address.chat = user;
    address.sender = user;
    return address;
}

inline im::Address group_address(const std::string& group, const std::string& member,
                                 const std::string& platform = "telegram", const std::string& account = "100") {
    im::Address address;
    address.platform = platform;
    address.account = account;
    address.kind = im::ChatKind::Group;
    address.chat = group;
    address.sender = member;
    return address;
}

inline im::Inbound message(const im::Address& address, const std::string& text, const std::string& id,
                           bool mentioned = true) {
    im::Inbound inbound;
    inbound.address = address;
    inbound.text = text;
    inbound.message_id = id;
    inbound.mentioned = address.kind == im::ChatKind::Private ? true : mentioned;
    inbound.sender_name = "名字-" + address.sender;
    inbound.reply_context = {{"msg_id", id}};
    return inbound;
}

} // namespace acecode::channels::core::test
