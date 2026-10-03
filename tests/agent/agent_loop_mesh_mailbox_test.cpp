#include "test_support/agent/agent_loop_fixture.hpp"
#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "permissions/permissions.hpp"
#include "session/inter_agent_message.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/tool_executor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// 蜂群模式（网状）agent 间邮件在 AgentLoop 里的投递时机(add-mesh-swarm-mode,
// 对齐 Codex InputQueue 的 mailbox 语义):
//   - 回合进行中到达的邮件在下一次模型请求前并入同一回合;
//   - 空闲时 trigger_turn 邮件(agent_followup_task / NEW_TASK)唤醒一个新回合,
//     非触发邮件(agent_send_message / FINAL_ANSWER)只排队等下一回合;
//   - agent_wait 的等待被新邮件或用户插话提前结束。

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using acecode::mesh::InterAgentEnvelope;
using acecode::mesh::InterAgentMessageType;

namespace {

fs::path mailbox_temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_mesh_mailbox_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

acecode::UserInput envelope_input(InterAgentMessageType type, const std::string& payload) {
    InterAgentEnvelope envelope;
    envelope.type = type;
    envelope.sender = "/root/peer";
    envelope.recipient = "/root/me";
    envelope.payload = payload;
    acecode::UserInput input;
    input.text = acecode::mesh::render_inter_agent_message(envelope);
    input.metadata = nlohmann::json::object();
    input.metadata[acecode::mesh::kInterAgentMetadataKey] =
        acecode::mesh::inter_agent_metadata(envelope);
    return input;
}

class MailboxHarness {
public:
    explicit MailboxHarness(const std::string& hint) : cwd_(mailbox_temp_cwd(hint)) {
        sm_->start_session(cwd_.string(), "stub", "stub-1", "sid-mesh-" + hint);
        acecode::ToolImpl probe;
        probe.definition.name = "probe";
        probe.definition.description = "Test probe";
        probe.definition.parameters = {{"type", "object"}, {"properties", nlohmann::json::object()}};
        probe.is_read_only = true;
        probe.execute = [this](const std::string&, const acecode::ToolContext&) {
            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> lk(mu_);
                hook = on_probe_;
            }
            if (hook) hook();
            return acecode::ToolResult{"probe ok", true};
        };
        tools_.register_tool(std::move(probe));

        acecode::AgentCallbacks callbacks;
        callbacks.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(mu_);
            busy_ = busy;
            cv_.notify_all();
        };
        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; };
        loop_ = std::make_unique<acecode::AgentLoop>(
            acecode_test::AgentLoopFixture::dependencies(accessor, tools_, callbacks,
                                                         permissions_, sm_.get()),
            acecode_test::AgentLoopFixture::configuration(cwd_.string()));
        loop_->start();
        sub_ = loop_->events().subscribe([this](const acecode::SessionEvent& event) {
            std::lock_guard<std::mutex> lk(events_mu_);
            events_.push_back(event);
        });
    }

    ~MailboxHarness() {
        if (loop_ && sub_ != 0) loop_->events().unsubscribe(sub_);
        loop_.reset();
        sm_.reset();
        fs::remove_all(cwd_);
        fs::remove_all(acecode::SessionStorage::get_project_dir(cwd_.string()));
    }

    acecode::AgentLoop& loop() { return *loop_; }
    acecode_test::StubLlmProvider& provider() { return *provider_; }
    acecode::SessionManager& session() { return *sm_; }

    void on_probe(std::function<void()> hook) {
        std::lock_guard<std::mutex> lk(mu_);
        on_probe_ = std::move(hook);
    }

    // 等到 provider 收到 count 次请求且 loop 回到空闲(连续两次观测,排除回合间隙)。
    bool wait_turns_and_idle(int count, std::chrono::milliseconds timeout = 10s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (provider_->turn_count() >= count && !loop_->is_busy()) {
                std::this_thread::sleep_for(30ms);
                if (provider_->turn_count() >= count && !loop_->is_busy()) return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return false;
    }

    std::vector<acecode::SessionEvent> events() const {
        std::lock_guard<std::mutex> lk(events_mu_);
        return events_;
    }

private:
    fs::path cwd_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    acecode::ToolExecutor tools_;
    acecode::PermissionManager permissions_;
    std::unique_ptr<acecode::SessionManager> sm_ = std::make_unique<acecode::SessionManager>();
    std::unique_ptr<acecode::AgentLoop> loop_;
    acecode::EventDispatcher::SubscriptionId sub_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
    bool busy_ = false;
    std::function<void()> on_probe_;
    mutable std::mutex events_mu_;
    std::vector<acecode::SessionEvent> events_;
};

std::vector<std::string> user_texts(const std::vector<acecode::ChatMessage>& messages) {
    std::vector<std::string> texts;
    for (const auto& message : messages) {
        if (message.role == "user") texts.push_back(message.content);
    }
    return texts;
}

bool contains_text(const std::vector<std::string>& texts, const std::string& needle) {
    for (const auto& text : texts) {
        if (text == needle) return true;
    }
    return false;
}

} // namespace

// 场景:agent 空闲时收到 agent_followup_task(trigger_turn=true)。
// 期望:邮件本身作为新回合的输入唤醒一次模型请求;落盘消息带 inter_agent 元数据,
// 并发出 role=user 的 Message 事件(Web 据此渲染 agent 间消息行)。
TEST(AgentLoopMeshMailbox, TriggerMailWakesIdleAgent) {
    MailboxHarness h("trigger-idle");
    h.provider().push_text("on it");
    const auto mail = envelope_input(InterAgentMessageType::NewTask, "investigate");
    h.loop().deliver_inter_agent_message(mail, true);
    ASSERT_TRUE(h.wait_turns_and_idle(1));
    EXPECT_EQ(h.provider().turn_count(), 1);

    const auto request = user_texts(h.provider().messages_for_turn(0));
    ASSERT_FALSE(request.empty());
    EXPECT_EQ(request.back(), mail.text);
    EXPECT_EQ(h.loop().pending_mailbox_count(), 0u);

    const auto persisted = h.session().load_active_messages();
    const auto it = std::find_if(persisted.begin(), persisted.end(), [](const auto& message) {
        return acecode::mesh::is_inter_agent_message(message);
    });
    ASSERT_NE(it, persisted.end());
    EXPECT_EQ(it->content, mail.text);

    bool saw_event = false;
    for (const auto& event : h.events()) {
        if (event.kind == acecode::SessionEventKind::Message &&
            event.payload.value("role", std::string{}) == "user" &&
            event.payload.contains("metadata") &&
            event.payload["metadata"].contains(acecode::mesh::kInterAgentMetadataKey)) {
            saw_event = true;
        }
    }
    EXPECT_TRUE(saw_event);
}

// 场景:agent 空闲时收到 agent_send_message(trigger_turn=false)。
// 期望:不开新回合,邮件留在邮箱;下一次用户输入开回合时,首个模型请求前并入
// (用户消息在前、信封在后),邮箱清空。回归:若非触发邮件也唤醒回合,
// 子 agent 的每条 FINAL_ANSWER 都会让空闲的父 agent 自动开工(Codex 明确不唤醒)。
TEST(AgentLoopMeshMailbox, QueueOnlyMailWaitsForNextTurn) {
    MailboxHarness h("queue-only");
    const auto mail = envelope_input(InterAgentMessageType::Message, "fyi");
    h.loop().deliver_inter_agent_message(mail, false);
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(h.provider().turn_count(), 0);
    EXPECT_EQ(h.loop().pending_mailbox_count(), 1u);
    EXPECT_FALSE(h.loop().has_pending_trigger_mail());

    h.provider().push_text("answer");
    h.loop().submit("next question");
    ASSERT_TRUE(h.wait_turns_and_idle(1));
    const auto request = user_texts(h.provider().messages_for_turn(0));
    ASSERT_GE(request.size(), 2u);
    EXPECT_EQ(request[request.size() - 2], "next question");
    EXPECT_EQ(request.back(), mail.text);
    EXPECT_EQ(h.loop().pending_mailbox_count(), 0u);
}

// 场景:回合进行中(工具执行期间)收到邮件,触发与非触发各一次。
// 期望:邮件在工具结果之后、下一次模型请求之前并入同一回合;回合结束后不会因
// 已被消费的触发邮件再多跑一个回合(provider 总请求数恰好 2)。
TEST(AgentLoopMeshMailbox, MailArrivingMidTurnJoinsTheNextRequest) {
    for (const bool trigger : {false, true}) {
        SCOPED_TRACE(trigger ? "trigger" : "queue-only");
        MailboxHarness h(trigger ? "midturn-trigger" : "midturn-queue");
        const auto mail = envelope_input(InterAgentMessageType::Message, "mid-turn note");
        h.on_probe([&h, mail, trigger] { h.loop().deliver_inter_agent_message(mail, trigger); });
        h.provider().push_tool_call("probe", "{}", "call-probe");
        h.provider().push_text("finished");
        h.loop().submit("start");
        ASSERT_TRUE(h.wait_turns_and_idle(2));
        std::this_thread::sleep_for(150ms);
        EXPECT_EQ(h.provider().turn_count(), 2);
        EXPECT_FALSE(contains_text(user_texts(h.provider().messages_for_turn(0)), mail.text));
        const auto second = h.provider().messages_for_turn(1);
        ASSERT_FALSE(second.empty());
        EXPECT_EQ(second.back().role, "user");
        EXPECT_EQ(second.back().content, mail.text);
        EXPECT_EQ(h.loop().pending_mailbox_count(), 0u);
    }
}

// 场景:agent_wait 的三种结局。
// 期望:已有待收邮件立即返回 Mailbox;无活动按超时返回 TimedOut;等待中另一线程
// 投递邮件返回 Mailbox;中止标志置位返回 Aborted,且远早于截止时间。
TEST(AgentLoopMeshMailbox, WaitForMailboxActivityOutcomes) {
    MailboxHarness h("wait");
    using Outcome = acecode::AgentLoop::MailboxWaitOutcome;
    EXPECT_EQ(h.loop().wait_for_mailbox_activity(30ms, nullptr), Outcome::TimedOut);

    std::thread sender([&h] {
        std::this_thread::sleep_for(50ms);
        h.loop().deliver_inter_agent_message(envelope_input(InterAgentMessageType::Message, "x"),
                                             false);
    });
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(h.loop().wait_for_mailbox_activity(10s, nullptr), Outcome::Mailbox);
    sender.join();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);

    // 邮件未被消费时,再次等待立即返回(对应 Codex:已有待收邮件不阻塞)。
    EXPECT_EQ(h.loop().wait_for_mailbox_activity(10s, nullptr), Outcome::Mailbox);

    MailboxHarness idle("wait-abort");
    std::atomic<bool> abort{false};
    std::thread aborter([&abort] {
        std::this_thread::sleep_for(50ms);
        abort.store(true);
    });
    const auto abort_started = std::chrono::steady_clock::now();
    EXPECT_EQ(idle.loop().wait_for_mailbox_activity(10s, &abort), Outcome::Aborted);
    aborter.join();
    EXPECT_LT(std::chrono::steady_clock::now() - abort_started, 5s);
}
