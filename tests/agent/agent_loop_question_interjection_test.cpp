#include "test_support/agent/agent_loop_fixture.hpp"
// 覆盖 AgentLoop::interject_question:AskUserQuestion 挂起时用户没作答而是
// 直接发了一条文本(Web 输入框 / IM 通道的普通文本)。
//
// 修复前的真实症状(时序问题):
//   - Web:提问期间输入框被禁用,用户只能先点「取消回答」,工具立刻返回
//     "[Error] User declined to answer questions.",模型带着「用户拒答」继续
//     自作主张;用户随后打的那句话排在队列里,要等本回合结束才作为新回合
//     送达 —— 模型已经按错误前提走了一大截。
//   - IM 通道:非 /aq 文本直接 send_input,排在被问题阻塞的回合后面,问题
//     却还在等;用户以为答过了,双方互相等到超时。
//
// 现在的契约(三条,任一回归都是用户可见的行为倒退):
//   1. 问题以「用户改为直接输入」收掉:工具 success=true,output 以
//      "[User interjected]" 开头,metadata.ask_user_question_result.interjected;
//      question_closed.reason == "interjected"。
//   2. 文本作为同回合 steering user 消息紧跟在该工具结果之后进入模型上下文,
//      带 turn_steer / question_interjection / question_request_id 元数据;
//      回合不 abort、不出现 [Interjected] 通知与 <turn_aborted> 标记。
//   3. 问题已不再挂起(未知 / 已回答 / 重复插话)→ NoPendingQuestion,文本
//      不会被静默提交成普通 steer,调用方据此退回普通发送路径。

#include "session/session_client.hpp"
#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "permissions/permissions.hpp"
#include "llm/llm_provider.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/event_dispatcher.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/ask_user_question_tool.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

std::string make_ask_args() {
    nlohmann::json questions = nlohmann::json::array({
        {
            {"question", "Which http client should we use?"},
            {"header", "Library"},
            {"options", nlohmann::json::array({
                {{"label", "axios (Recommended)"}, {"description", "popular"}},
                {{"label", "fetch"}, {"description", "native"}},
            })},
            {"multiSelect", false},
        },
    });
    return nlohmann::json{{"questions", std::move(questions)}}.dump();
}

// 装真实 prompter + 真实 AskUserQuestion 工具;"前端" listener 只记录
// QuestionRequest,不自动作答 —— 由测试主线程决定是插话还是作答。
class InterjectionHarness {
public:
    InterjectionHarness() {
        tools_.register_tool(acecode::create_ask_user_question_tool_async());

        acecode::AgentCallbacks cb;
        cb.on_tool_result = [this](const acecode::ChatMessage&,
                                   const std::string& tool_name,
                                   const acecode::ToolResult& result) {
            std::lock_guard<std::mutex> lk(mu_);
            tool_results_.push_back({tool_name, result});
        };
        cb.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(mu_);
            busy_ = busy;
            saw_busy_ = saw_busy_ || busy;
            cv_.notify_all();
        };
        cb.on_tool_confirm = [](const std::string&, const std::string&) {
            return acecode::PermissionResult::Allow;
        };

        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> {
            return provider_;
        };
        loop_ = std::make_unique<acecode::AgentLoop>(
        acecode_test::AgentLoopFixture::dependencies(accessor, tools_, cb, perms_),
        acecode_test::AgentLoopFixture::configuration(/*cwd=*/"."));
        auto owned_prompter = std::make_unique<acecode::AskUserQuestionPrompter>(
            loop_->events());
        prompter_ = owned_prompter.get();
        loop_->set_ask_question_prompter(std::move(owned_prompter));
        sub_ = loop_->events().subscribe([this](const acecode::SessionEvent& e) {
            std::lock_guard<std::mutex> lk(mu_);
            events_.push_back(e);
            if (e.kind == acecode::SessionEventKind::QuestionRequest) {
                request_ids_.push_back(
                    e.payload.value("request_id", std::string{}));
            }
            cv_.notify_all();
        });
        loop_->start();
    }

    ~InterjectionHarness() {
        if (sub_ != 0) loop_->events().unsubscribe(sub_);
        loop_.reset();
    }

    acecode::AgentLoop& loop() { return *loop_; }
    acecode_test::StubLlmProvider& provider() { return *provider_; }
    acecode::AskUserQuestionPrompter& prompter() { return *prompter_; }

    // 等到第 index 个 QuestionRequest 到达,返回其 request_id。
    std::string wait_for_request(std::size_t index = 0,
                                 std::chrono::milliseconds timeout = 5s) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, timeout, [&] {
                return request_ids_.size() > index;
            })) {
            return {};
        }
        return request_ids_[index];
    }

    bool wait_until_idle(std::chrono::milliseconds timeout = 10s) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [this] {
            return saw_busy_ && !busy_;
        });
    }

    struct ToolCallResult {
        std::string tool_name;
        acecode::ToolResult result;
    };

    std::vector<ToolCallResult> tool_results() {
        std::lock_guard<std::mutex> lk(mu_);
        return tool_results_;
    }

    std::vector<acecode::SessionEvent> events() {
        std::lock_guard<std::mutex> lk(mu_);
        return events_;
    }

private:
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    acecode::ToolExecutor tools_;
    acecode::PermissionManager perms_;
    std::unique_ptr<acecode::AgentLoop> loop_;
    acecode::AskUserQuestionPrompter* prompter_ = nullptr; // Borrowed from loop.
    acecode::EventDispatcher::SubscriptionId sub_ = 0;

    std::mutex mu_;
    std::condition_variable cv_;
    bool busy_ = false;
    bool saw_busy_ = false;
    std::vector<std::string> request_ids_;
    std::vector<ToolCallResult> tool_results_;
    std::vector<acecode::SessionEvent> events_;
};

const InterjectionHarness::ToolCallResult* find_ask_result(
    const std::vector<InterjectionHarness::ToolCallResult>& results) {
    for (const auto& r : results) {
        if (r.tool_name == "AskUserQuestion") return &r;
    }
    return nullptr;
}

} // namespace

// 场景:模型调 AskUserQuestion 后用户直接发文本插话。
// 期望:问题以 interjected 收掉、文本紧跟工具结果进入同一回合、回合不中断。
TEST(AgentLoopQuestionInterjection, ResolvesQuestionAndContinuesSameTurn) {
    InterjectionHarness h;
    h.provider().push_tool_call("AskUserQuestion", make_ask_args(), "ask-1");
    h.provider().push_text("ok, using fetch");

    h.loop().submit("pick a library for me");
    const std::string rid = h.wait_for_request();
    ASSERT_FALSE(rid.empty()) << "前端应收到 QuestionRequest";
    const std::string turn_id = h.loop().active_turn_id();
    ASSERT_FALSE(turn_id.empty());

    acecode::UserInput interjection;
    interjection.text = "别问了,直接用 fetch";
    interjection.metadata["client_message_id"] = "interject-1";
    const auto result = h.loop().interject_question(rid, interjection);
    ASSERT_EQ(result.status, acecode::TurnSteerStatus::Accepted) << result.message;
    EXPECT_EQ(result.turn_id, turn_id);

    ASSERT_TRUE(h.wait_until_idle());
    ASSERT_EQ(h.provider().turn_count(), 2)
        << "插话必须在同一回合内继续,不能开出第三次模型调用(新回合)";

    // 契约 1:工具结果是「用户改为直接输入」,不是 declined 错误。
    // (快照先落到局部变量,find_ask_result 返回的是指向它内部的指针。)
    const auto results = h.tool_results();
    const auto* ask = find_ask_result(results);
    ASSERT_NE(ask, nullptr);
    EXPECT_TRUE(ask->result.success) << ask->result.output;
    EXPECT_EQ(ask->result.output.rfind("[User interjected]", 0), 0u)
        << ask->result.output;
    EXPECT_EQ(ask->result.output.find("User declined"), std::string::npos);
    ASSERT_TRUE(ask->result.metadata.contains("ask_user_question_result"));
    EXPECT_TRUE(ask->result.metadata["ask_user_question_result"]
                    .value("interjected", false));

    // 契约 2:第二次模型请求里,顺序恒为 tool_call → tool_result → 插话 user 消息,
    // 且插话是最后一条(模型据此继续)。
    const auto second_request = h.provider().messages_for_turn(1);
    ASSERT_FALSE(second_request.empty());
    std::size_t tool_index = second_request.size();
    std::size_t steer_index = second_request.size();
    for (std::size_t i = 0; i < second_request.size(); ++i) {
        const auto& m = second_request[i];
        if (m.role == "tool" && tool_index == second_request.size()) {
            tool_index = i;
        }
        if (m.role == "user" && m.content == "别问了,直接用 fetch") {
            steer_index = i;
        }
    }
    ASSERT_LT(tool_index, second_request.size()) << "缺工具结果";
    ASSERT_LT(steer_index, second_request.size()) << "插话没进入模型上下文";
    EXPECT_LT(tool_index, steer_index) << "插话必须排在工具结果之后";
    EXPECT_EQ(steer_index, second_request.size() - 1)
        << "插话应是本次请求的最后一条 user 消息";
    const auto& steer = second_request[steer_index];
    EXPECT_TRUE(steer.metadata.value("turn_steer", false));
    EXPECT_TRUE(steer.metadata.value("question_interjection", false));
    EXPECT_EQ(steer.metadata.value("question_request_id", ""), rid);
    EXPECT_EQ(steer.metadata.value("client_message_id", ""), "interject-1");
    EXPECT_EQ(steer.metadata.value("turn_id", ""), turn_id);

    // 回合没有被打断:无 <turn_aborted> 标记、无 [Interjected] 通知,
    // 结束态是 completed;question_closed 的 reason 是 interjected。
    for (const auto& m : h.loop().messages()) {
        EXPECT_FALSE(m.metadata.is_object() &&
                     m.metadata.value("turn_interrupt_marker", false));
    }
    bool saw_interjected_notice = false;
    bool saw_completed = false;
    std::string close_reason;
    for (const auto& e : h.events()) {
        if (e.kind == acecode::SessionEventKind::Message &&
            e.payload.value("role", "") == "system" &&
            e.payload.value("content", "") == "[Interjected]") {
            saw_interjected_notice = true;
        }
        if (e.kind == acecode::SessionEventKind::BusyChanged &&
            !e.payload.value("busy", true) &&
            e.payload.value("outcome", "") == "completed") {
            saw_completed = true;
        }
        if (e.kind == acecode::SessionEventKind::QuestionClosed &&
            e.payload.value("request_id", "") == rid) {
            close_reason = e.payload.value("reason", "");
        }
    }
    EXPECT_FALSE(saw_interjected_notice);
    EXPECT_TRUE(saw_completed);
    EXPECT_EQ(close_reason, "interjected");
}

// 场景:问题挂起时用一个不存在的 request_id 插话,然后正常作答。
// 期望:返回 NoPendingQuestion,文本不会被偷偷提交成普通 steer;正常作答后
// 模型看到的是真实答案。回归:若插话在问题收掉失败后仍压入 steer 队列,
// 用户会在前端退回排队/直接发送后收到两份同一条消息。
TEST(AgentLoopQuestionInterjection, UnknownRequestIsRejectedWithoutCommittingInput) {
    InterjectionHarness h;
    h.provider().push_tool_call("AskUserQuestion", make_ask_args(), "ask-1");
    h.provider().push_text("done");

    h.loop().submit("ask me");
    const std::string rid = h.wait_for_request();
    ASSERT_FALSE(rid.empty());

    acecode::UserInput interjection;
    interjection.text = "this must never reach the model";
    const auto rejected = h.loop().interject_question("no-such-request", interjection);
    EXPECT_EQ(rejected.status, acecode::TurnSteerStatus::NoPendingQuestion);
    EXPECT_EQ(h.prompter().pending_count(), 1u) << "问题必须仍然挂起";

    acecode::AskUserQuestionResponse answer;
    acecode::AskUserQuestionAnswer item;
    item.question_id = "Which http client should we use?";
    item.selected = {"fetch"};
    answer.answers.push_back(item);
    ASSERT_TRUE(h.prompter().notify_response(rid, answer));
    ASSERT_TRUE(h.wait_until_idle());

    for (const auto& m : h.loop().messages()) {
        EXPECT_NE(m.content, "this must never reach the model");
    }
    const auto results = h.tool_results();
    const auto* ask = find_ask_result(results);
    ASSERT_NE(ask, nullptr);
    EXPECT_TRUE(ask->result.success);
    EXPECT_NE(ask->result.output.find("\"fetch\""), std::string::npos)
        << ask->result.output;
}

// 场景:同一个问题被插话两次(用户连按两次发送 / 两个客户端同时插话)。
// 期望:first-wins,第二次返回 NoPendingQuestion,模型上下文里只有一条插话。
TEST(AgentLoopQuestionInterjection, SecondInterjectionForSameRequestIsRejected) {
    InterjectionHarness h;
    h.provider().push_tool_call("AskUserQuestion", make_ask_args(), "ask-1");
    h.provider().push_text("done");

    h.loop().submit("ask me");
    const std::string rid = h.wait_for_request();
    ASSERT_FALSE(rid.empty());

    acecode::UserInput first;
    first.text = "first interjection";
    acecode::UserInput second;
    second.text = "second interjection";
    ASSERT_TRUE(h.loop().interject_question(rid, first).accepted());
    EXPECT_EQ(h.loop().interject_question(rid, second).status,
              acecode::TurnSteerStatus::NoPendingQuestion);
    ASSERT_TRUE(h.wait_until_idle());

    int first_count = 0;
    for (const auto& m : h.loop().messages()) {
        if (m.content == "first interjection") ++first_count;
        EXPECT_NE(m.content, "second interjection");
    }
    EXPECT_EQ(first_count, 1);
}

// 场景:参数校验 —— 空文本、空 request_id、空闲会话、expected_turn_id 不匹配。
// 期望:分别 InvalidInput / InvalidInput / NoActiveTurn / TurnMismatch,且
// 不匹配的那次不会把问题收掉(用户随后仍能正常作答)。
TEST(AgentLoopQuestionInterjection, RejectsInvalidArgumentsAndTurnMismatch) {
    InterjectionHarness h;

    acecode::UserInput blank;
    blank.text = " \t\n";
    EXPECT_EQ(h.loop().interject_question("rid", blank).status,
              acecode::TurnSteerStatus::InvalidInput);

    acecode::UserInput text;
    text.text = "hello";
    EXPECT_EQ(h.loop().interject_question("", text).status,
              acecode::TurnSteerStatus::InvalidInput);
    EXPECT_EQ(h.loop().interject_question("rid", text).status,
              acecode::TurnSteerStatus::NoActiveTurn);

    h.provider().push_tool_call("AskUserQuestion", make_ask_args(), "ask-1");
    h.provider().push_text("done");
    h.loop().submit("ask me");
    const std::string rid = h.wait_for_request();
    ASSERT_FALSE(rid.empty());

    const auto mismatch = h.loop().interject_question(rid, text, "some-other-turn");
    EXPECT_EQ(mismatch.status, acecode::TurnSteerStatus::TurnMismatch);
    EXPECT_EQ(mismatch.turn_id, h.loop().active_turn_id());
    EXPECT_EQ(h.prompter().pending_count(), 1u)
        << "turn 不匹配时问题必须原样挂起";

    acecode::AskUserQuestionResponse cancel;
    cancel.cancelled = true;
    ASSERT_TRUE(h.prompter().notify_response(rid, cancel));
    ASSERT_TRUE(h.wait_until_idle());
    for (const auto& m : h.loop().messages()) {
        EXPECT_NE(m.content, "hello");
    }
}
