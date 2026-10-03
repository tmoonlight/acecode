#include <gtest/gtest.h>

#include "agent/mailbox/agent_mailbox.hpp"

#include <atomic>
#include <chrono>
#include <thread>

// 蜂群模式（网状）的会话级邮箱(add-mesh-swarm-mode,对应 Codex
// InputQueue::mailbox_pending_mails)。agent_wait 阻塞在 wait() 上,
// agent_send_message / agent_followup_task 经 push() 唤醒它,用户插话经
// notify_steer() 唤醒它。

using acecode::agent::AgentMailbox;
using acecode::agent::MailboxActivity;
using namespace std::chrono_literals;

namespace {

AgentMailbox::Mail mail(const std::string& text, bool trigger) {
    AgentMailbox::Mail item;
    item.input.text = text;
    item.trigger_turn = trigger;
    return item;
}

} // namespace

// 场景:邮件按到达顺序排队,followup_task 带 trigger_turn。
// 期望:FIFO 出队;has_trigger_turn 只要队里任意一封带触发就为真;take_all 清空。
TEST(MeshAgentMailbox, QueuesInOrderAndTracksTriggerMail) {
    AgentMailbox box;
    box.push(mail("a", false));
    EXPECT_FALSE(box.has_trigger_turn());
    box.push(mail("b", true));
    EXPECT_TRUE(box.has_trigger_turn());
    EXPECT_EQ(box.size(), 2u);

    AgentMailbox::Mail first;
    ASSERT_TRUE(box.pop_front(first));
    EXPECT_EQ(first.input.text, "a");
    auto rest = box.take_all();
    ASSERT_EQ(rest.size(), 1u);
    EXPECT_EQ(rest.front().input.text, "b");
    EXPECT_EQ(box.size(), 0u);
    EXPECT_FALSE(box.pop_front(first));
}

// 场景:agent_wait 先记下活动序号再等待;等待期间别的 agent 投递了邮件。
// 期望:wait 立刻以 Mailbox 返回,而不是睡到超时(Codex "Wait completed.")。
TEST(MeshAgentMailbox, WaitWakesOnNewMail) {
    AgentMailbox box;
    const auto mark = box.activity_sequence();
    std::thread sender([&box] {
        std::this_thread::sleep_for(50ms);
        box.push(mail("hello", false));
    });
    const auto started = std::chrono::steady_clock::now();
    const auto activity = box.wait(mark, started + 10s, nullptr);
    sender.join();
    EXPECT_EQ(activity, MailboxActivity::Mailbox);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
}

// 场景:agent_wait 期间用户向这个 agent 插话(steer)。
// 期望:wait 以 Steer 返回(对应 Codex "Wait interrupted by new input.")。
TEST(MeshAgentMailbox, WaitReportsSteerActivity) {
    AgentMailbox box;
    const auto mark = box.activity_sequence();
    box.notify_steer();
    EXPECT_EQ(box.wait(mark, std::chrono::steady_clock::now() + 1s, nullptr),
              MailboxActivity::Steer);
}

// 场景:没有任何活动、回合被中止、会话关闭三种退出路径。
// 期望:超时 / abort / close 都返回 None 且不会挂死;abort 靠 100ms 轮询观察到,
// 远早于 10s 截止时间(否则停止按钮在 agent_wait 期间失效)。
TEST(MeshAgentMailbox, WaitReturnsNoneOnTimeoutAbortAndClose) {
    AgentMailbox box;
    const auto mark = box.activity_sequence();
    EXPECT_EQ(box.wait(mark, std::chrono::steady_clock::now() + 30ms, nullptr),
              MailboxActivity::None);

    std::atomic<bool> abort{false};
    std::thread aborter([&abort] {
        std::this_thread::sleep_for(50ms);
        abort.store(true);
    });
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(box.wait(mark, started + 10s, &abort), MailboxActivity::None);
    aborter.join();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);

    box.close();
    EXPECT_EQ(box.wait(mark, std::chrono::steady_clock::now() + 10s, nullptr),
              MailboxActivity::None);
}
