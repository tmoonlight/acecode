#include "test_support/agent/agent_loop_fixture.hpp"
// PA 兜底(src/pa/pa_overflow_rescue)的端到端用例:服务端以 PA 特征报文
// 「请求上下文过大」拒收整个请求时,AgentLoop 原样重发 → 逐档收缩 → 紧急档
// → 等待重发,绝不因为这条报文终止回合。等待全部按 0 缩放,用例只验证顺序
// 与效果,不验证真实时长。
#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "pa/pa_context_budget.hpp"
#include "pa/pa_overflow_rescue.hpp"
#include "permissions/permissions.hpp"
#include "llm/llm_provider.hpp"
#include "session/session_manager.hpp"
#include "session/session_client.hpp"
#include "session/session_storage.hpp"
#include "session/thread_repair.hpp"
#include "tool/mtime_tracker.hpp"
#include "tool/tool_executor.hpp"
#include "test_support/agent/stub_provider.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

// 等待缩放系数是进程级的,用例之间必须还原,否则后面的用例会真的睡几秒。
// 学习器观测表同样是进程级单例:这里的历史规模接近可信下限,一旦记进去会把
// 同一 (provider, model) 身份下所有 Stub 用例的压缩窗口砍小,所以前后都清空。
class RescueWaitGuard {
public:
    RescueWaitGuard() {
        acecode::pa::set_rescue_wait_scale_for_test(0.0);
        acecode::pa::context_budget().reset();
    }
    ~RescueWaitGuard() {
        acecode::pa::set_rescue_wait_scale_for_test(1.0);
        acecode::pa::context_budget().reset();
    }
};

acecode::ChatMessage loop_msg(std::string role, std::string content) {
    acecode::ChatMessage message;
    message.role = std::move(role);
    message.content = std::move(content);
    return message;
}

void add_history(acecode::AgentLoop& loop, int turns) {
    for (int i = 0; i < turns; ++i) {
        loop.push_message(loop_msg(
            "user", "old user " + std::to_string(i) + " " +
                        std::string(900, 'u')));
        loop.push_message(loop_msg(
            "assistant", "old assistant " + std::to_string(i) + " " +
                             std::string(900, 'a')));
    }
}

// 线上实测报文,一字不改(与 tests/pa/pa_quirks_test.cpp 同源)。
acecode::ProviderErrorInfo make_pa_overflow_error() {
    acecode::ProviderErrorInfo info;
    info.kind = acecode::ProviderErrorKind::Http;
    info.status_code = 400;
    info.display_message = "HTTP 400 from openai model aicoder-pro";
    info.raw_body =
        R"({"object":"error","message":"请求上下文过大",)"
        R"("type":"BadRequestError","code":400})";
    info.body_is_json = true;
    return info;
}

bool request_contains(const std::vector<acecode::ChatMessage>& messages,
                      const std::string& needle) {
    for (const auto& message : messages) {
        if (message.content.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::filesystem::path make_temp_cwd(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() /
                ("acecode_" + name + "_" +
                 std::to_string(std::random_device{}()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::vector<acecode::SessionEvent> wait_for_done(
    acecode::AgentLoop& loop,
    const std::function<void()>& action) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::vector<acecode::SessionEvent> events;
    const auto subscription = loop.events().subscribe(
        [&](const acecode::SessionEvent& event) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
            if (event.kind == acecode::SessionEventKind::Done) {
                done = true;
                cv.notify_all();
            }
        });
    action();
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, 20s, [&] { return done; }));
    }
    loop.events().unsubscribe(subscription);
    return events;
}

int count_message_events(const std::vector<acecode::SessionEvent>& events,
                         const std::string& role,
                         const std::string& needle) {
    int count = 0;
    for (const auto& event : events) {
        if (event.kind == acecode::SessionEventKind::Message &&
            event.payload.value("role", "") == role &&
            event.payload.value("content", "").find(needle) !=
                std::string::npos) {
            ++count;
        }
    }
    return count;
}

bool has_system_event(const std::vector<acecode::SessionEvent>& events,
                      const std::string& needle) {
    return count_message_events(events, "system", needle) > 0;
}

bool has_error_event(const std::vector<acecode::SessionEvent>& events) {
    return count_message_events(events, "error", "") > 0;
}

void register_read_only_tool(acecode::ToolExecutor& tools,
                             const std::string& name,
                             std::string output,
                             std::string description = "test tool") {
    acecode::ToolImpl tool;
    tool.definition.name = name;
    tool.definition.description = std::move(description);
    tool.definition.parameters = nlohmann::json{
        {"type", "object"},
        {"properties", nlohmann::json::object()},
    };
    tool.is_read_only = true;
    tool.execute = [output](const std::string&, const acecode::ToolContext&) {
        return acecode::ToolResult{output, true};
    };
    tools.register_tool(std::move(tool));
}

// 修剪与清工具输出都要落 checkpoint,没有 SessionManager 会直接判 Failed,
// 兜底也就无从谈起 —— 每个用例都挂一个真实会话。
struct RescueHarness {
    std::shared_ptr<acecode_test::StubLlmProvider> provider =
        std::make_shared<acecode_test::StubLlmProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    std::filesystem::path cwd;
    std::string project_dir;
    acecode::SessionManager session;
    acecode::AgentLoop loop;

    explicit RescueHarness(const std::string& name)
        : cwd(make_temp_cwd(name)),
          project_dir(acecode::SessionStorage::get_project_dir(cwd.string())),
          loop(
        acecode_test::AgentLoopFixture::dependencies([this]() -> std::shared_ptr<acecode::LlmProvider> {
                   return provider;
               }, tools, {}, permissions, &session),
        acecode_test::AgentLoopFixture::configuration(cwd.string())) {
        loop.start();
        std::filesystem::remove_all(project_dir);
        session.start_session(cwd.string(), "stub", "model");
        // 窗口开到足够大,让自动压缩永远不触发,用例只看拒收后的兜底路径。
        loop.set_context_window(1000000);
    }

    ~RescueHarness() {
        loop.shutdown();
        session.finalize();
        std::error_code ec;
        std::filesystem::remove_all(project_dir, ec);
        std::filesystem::remove_all(cwd, ec);
    }

    void push_pa_errors(int count) {
        for (int i = 0; i < count; ++i) {
            provider->push_error(make_pa_overflow_error());
        }
    }
};

} // namespace

// 触发场景:服务端第一次以 PA 报文拒收,紧接着的原样重发就被接受(抽风)。
// 期望行为:第二次请求与第一次逐条相同,历史一条都没动,没有错误事件,也
// 没有走到收缩。
// 回归背景:旧恢复链一被拒就修剪历史,一次抽风就白丢一组回合。
TEST(AgentLoopPaRescue, RetriesTheSameRequestBeforeTouchingHistory) {
    RescueWaitGuard wait_guard;
    RescueHarness h("pa_rescue_same_request");
    add_history(h.loop, 3);
    h.push_pa_errors(1);
    h.provider->push_text("recovered response");

    const auto events = wait_for_done(h.loop, [&] {
        h.loop.submit("latest user request");
    });

    EXPECT_EQ(h.provider->turn_count(), 2);
    const auto first = h.provider->messages_for_turn(0);
    const auto second = h.provider->messages_for_turn(1);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].content, second[i].content) << "message " << i;
    }
    EXPECT_TRUE(request_contains(h.loop.messages(), "old assistant 0"));
    EXPECT_TRUE(request_contains(h.loop.messages(), "recovered response"));
    EXPECT_FALSE(has_error_event(events));
    EXPECT_TRUE(has_system_event(events, "先原样重发"));
    EXPECT_FALSE(has_system_event(events, "次收缩"));
}

// 触发场景:原样重发两次都被拒,之后每次收缩后的重发又被拒,连续两档。
// 期望行为:不设修复次数上限 —— 第 3 次被拒起每次被拒都再缩一档(先丢最旧
// 的整组回合),直到被接受;最终回复正常产出,没有错误事件。
// 回归背景:旧恢复链修剪一次、紧急档一次之后就报错终止。
TEST(AgentLoopPaRescue, ShrinksRoundAfterRoundUntilAccepted) {
    RescueWaitGuard wait_guard;
    RescueHarness h("pa_rescue_shrink_rounds");
    add_history(h.loop, 6);
    // 原样重发 2 次 + 收缩后重发 2 次,共 5 次被拒,第 6 次被接受。
    h.push_pa_errors(5);
    h.provider->push_text("accepted after shrinking");

    const auto events = wait_for_done(h.loop, [&] {
        h.loop.submit("latest user request");
    });

    EXPECT_EQ(h.provider->turn_count(), 6);
    EXPECT_FALSE(request_contains(h.loop.messages(), "old user 0"));
    EXPECT_FALSE(request_contains(h.loop.messages(), "old user 1"));
    EXPECT_TRUE(request_contains(h.loop.messages(), "latest user request"));
    EXPECT_TRUE(request_contains(h.loop.messages(), "accepted after shrinking"));
    EXPECT_FALSE(has_error_event(events));
    EXPECT_TRUE(has_system_event(events, "第 1 次收缩"));
    EXPECT_TRUE(has_system_event(events, "第 2 次收缩"));
}

// 触发场景:整个会话只有当前回合,回合里已经跑了两个工具、各返回一大段输出,
// 之后的请求被拒(原样重发也被拒)。
// 期望行为:老回合没得丢,就清本回合最旧的工具输出(占位符),最近一条工具
// 输出保留;重发被接受,回合正常结束。
// 回归背景:截图那次致命 400 正是这种形态 —— 单回合读了一堆大文件,旧恢复
// 链在「只剩当前回合」时直接判 HistoryExhausted,只剩紧急档一条路。
TEST(AgentLoopPaRescue, ClearsToolOutputsInsideTheCurrentTurn) {
    RescueWaitGuard wait_guard;
    RescueHarness h("pa_rescue_clear_outputs");
    const std::string output_a(3000, 'A');
    const std::string output_b(3000, 'B');
    register_read_only_tool(h.tools, "tool_a", output_a);
    register_read_only_tool(h.tools, "tool_b", output_b);
    h.provider->push_tool_call("tool_a", "{}", "call-a");
    h.provider->push_tool_call("tool_b", "{}", "call-b");
    h.push_pa_errors(3);
    h.provider->push_text("done after clearing");

    const auto events = wait_for_done(h.loop, [&] {
        h.loop.submit("read two big files");
    });

    EXPECT_EQ(h.provider->turn_count(), 6);
    const auto accepted_request = h.provider->messages_for_turn(5);
    EXPECT_TRUE(request_contains(accepted_request,
                                 acecode::kClearedToolOutputPlaceholder));
    EXPECT_FALSE(request_contains(accepted_request, output_a))
        << "最旧的工具输出应被占位符替换";
    EXPECT_TRUE(request_contains(accepted_request, output_b))
        << "最近一条工具输出必须保留";
    EXPECT_TRUE(request_contains(accepted_request, "read two big files"));
    EXPECT_TRUE(request_contains(h.loop.messages(), "done after clearing"));
    EXPECT_FALSE(has_error_event(events));
    EXPECT_TRUE(has_system_event(events, "清除 1 条旧工具输出"));
}

// 触发场景:会话里有一个老回合 —— 用户给出任务要求、模型调了一次工具拿回约
// 6 万字符的输出、再给出结论;当前回合的请求被 PA 连续拒收(原样重发也被拒)。
// 期望行为:收缩时先把那条旧工具输出换成占位符,老回合里的任务要求与结论都
// 留在被接受的请求里;回合正常结束,没有错误事件。
// 回归背景(反馈 LINDANDAN069,模型 aicoder-pro):旧顺序先整组丢老回合,
// 一次拒收就把最初的任务说明、用户的多次纠正和压缩摘要全部删掉,模型此后
// 「忘了」任务要求和刚总结出的做法,又去重写已验证过的函数。
// 6 万字符约 1.5 万 token:远大于测试里 system prompt 与工具表这些固定部分,
// 保证「只清这条输出」就能缩到 85% 目标以下,不用动老回合本身。
// 另外:收缩前本会话对某文件留有「已读观测」,收缩后必须清掉 —— 读取结果可能
// 刚被换成占位符,再读同一文件要拿到真实内容,而不是「文件未变、参见先前结果」。
TEST(AgentLoopPaRescue, ClearsOldToolOutputsBeforeDroppingEarlierInstructions) {
    RescueWaitGuard wait_guard;
    RescueHarness h("pa_rescue_keep_instructions");
    register_read_only_tool(h.tools, "tool_a", "unused");
    h.loop.push_message(loop_msg("user", "original task: verify every expect"));
    acecode::ChatMessage old_call = loop_msg("assistant", "");
    old_call.tool_calls = nlohmann::json::array({nlohmann::json{
        {"id", "old-call"},
        {"type", "function"},
        {"function", {{"name", "tool_a"}, {"arguments", "{}"}}},
    }});
    h.loop.push_message(old_call);
    const std::string old_output(60000, 'O');
    acecode::ChatMessage old_result = loop_msg("tool", old_output);
    old_result.tool_call_id = "old-call";
    h.loop.push_message(old_result);
    h.loop.push_message(loop_msg(
        "assistant", "old conclusion: reuse the validated helper"));
    // 当前回合先调一次工具:最近一条工具输出按规则受保护,旧输出才是可清的那条。
    register_read_only_tool(h.tools, "tool_b", "fresh result");
    h.provider->push_tool_call("tool_b", "{}", "call-b");
    h.push_pa_errors(3);
    h.provider->push_text("accepted without forgetting");
    const auto read_file = h.cwd / "already_read.txt";
    std::ofstream(read_file) << "content the model read earlier";
    const std::string scope = h.session.current_session_id();
    acecode::MtimeTracker::instance().record_read_observation(
        read_file.string(), 0, 0, false, 0, 0, scope);
    ASSERT_TRUE(acecode::MtimeTracker::instance().has_unchanged_read_observation(
        read_file.string(), 0, 0, false, 0, 0, scope));

    const auto events = wait_for_done(h.loop, [&] {
        h.loop.submit("latest user request");
    });

    EXPECT_FALSE(acecode::MtimeTracker::instance().has_unchanged_read_observation(
        read_file.string(), 0, 0, false, 0, 0, scope))
        << "历史被修复后,已读观测必须失效";
    // 1 次工具调用 + 3 次被拒 + 1 次被接受。
    ASSERT_EQ(h.provider->turn_count(), 5);
    const auto accepted_request = h.provider->messages_for_turn(4);
    EXPECT_TRUE(request_contains(accepted_request,
                                 "original task: verify every expect"))
        << "老回合的任务要求不能因为收缩而丢失";
    EXPECT_TRUE(request_contains(accepted_request,
                                 "old conclusion: reuse the validated helper"));
    EXPECT_FALSE(request_contains(accepted_request, old_output))
        << "旧工具输出应被占位符替换";
    EXPECT_TRUE(request_contains(accepted_request,
                                 acecode::kClearedToolOutputPlaceholder));
    EXPECT_TRUE(request_contains(accepted_request, "latest user request"));
    EXPECT_TRUE(request_contains(accepted_request, "fresh result"))
        << "最近一条工具输出必须保留";
    EXPECT_FALSE(has_error_event(events));
    EXPECT_TRUE(has_system_event(events, "已丢弃最旧的 0 组历史、清除 1 条"));
}

// 触发场景:历史里只有当前这一条用户输入,什么都缩不了,服务端仍连续拒收。
// 期望行为:收缩腾不出空间就改紧急档(工具定义被剥掉);紧急档也被拒就按退避
// 等待后原样重发,直到被接受;全程没有错误事件。
TEST(AgentLoopPaRescue, FallsBackToEmergencyProfileThenWaits) {
    RescueWaitGuard wait_guard;
    RescueHarness h("pa_rescue_emergency_wait");
    register_read_only_tool(h.tools, "large_optional_tool", "unused",
                            std::string(2000, 'd'));
    // 原样重发 2 次、紧急档 1 次、等待重发 1 次共 5 次被拒,第 6 次被接受。
    h.push_pa_errors(5);
    h.provider->push_text("accepted after waiting");

    const auto events = wait_for_done(h.loop, [&] {
        h.loop.submit("only current input");
    });

    EXPECT_EQ(h.provider->turn_count(), 6);
    EXPECT_EQ(h.provider->tools_for_turn(0).size(), 1u);
    EXPECT_EQ(h.provider->tools_for_turn(3).size(), 0u)
        << "第 4 次请求应是紧急档,非核心工具被剥掉";
    EXPECT_EQ(h.provider->tools_for_turn(5).size(), 0u)
        << "同一回合内紧急档保持";
    EXPECT_TRUE(request_contains(h.loop.messages(), "only current input"));
    EXPECT_TRUE(request_contains(h.loop.messages(), "accepted after waiting"));
    EXPECT_FALSE(has_error_event(events));
    EXPECT_TRUE(has_system_event(events, "精简请求档"));
    EXPECT_TRUE(has_system_event(events, "反复重试"));
}

// 触发场景:服务端连缩到最小的请求都一直拒收,直到等待次数耗尽。
// 期望行为:原样重发 2 次 + 紧急档 1 次 + 等待重发 12 次之后才放弃,错误
// 文案说明是等待耗尽;放弃之后不再发请求。这是「绝不终止」的唯一例外,而且
// 要等约 9.5 分钟才会走到。
TEST(AgentLoopPaRescue, GivesUpOnlyAfterWaitRetriesAreExhausted) {
    RescueWaitGuard wait_guard;
    RescueHarness h("pa_rescue_give_up");
    const int expected_requests = 1 + acecode::pa::PA_RESCUE_SAME_REQUEST_RETRIES +
                                  1 + acecode::pa::PA_RESCUE_MAX_WAIT_RETRIES;
    h.push_pa_errors(expected_requests);
    h.provider->push_text("must never be requested");

    const auto events = wait_for_done(h.loop, [&] {
        h.loop.submit("only current input");
    });

    EXPECT_EQ(h.provider->turn_count(), expected_requests);
    EXPECT_FALSE(request_contains(h.loop.messages(), "must never be requested"));
    EXPECT_EQ(count_message_events(events, "error", "等待重试后仍拒收"), 1);
}

// 触发场景(截图复现):回合里先撞一次墙、收缩后继续,又跑了一个工具,之后
// 再次撞墙。
// 期望行为:第二次撞墙同样走完整兜底(原样重发 → 收缩),回合正常结束。
// 回归背景:旧实现修剪一次之后就把回合内的恢复状态锁死,第二次撞墙直接进
// 紧急档、再拒就报错 —— 截图里的「thread repair and emergency profile were
// both exhausted」就是这条路径。
TEST(AgentLoopPaRescue, RecoversAgainAfterASuccessInTheSameTurn) {
    RescueWaitGuard wait_guard;
    RescueHarness h("pa_rescue_second_episode");
    add_history(h.loop, 6);
    register_read_only_tool(h.tools, "tool_a", std::string(3000, 'A'));
    h.push_pa_errors(3);
    h.provider->push_tool_call("tool_a", "{}", "call-a");
    h.push_pa_errors(3);
    h.provider->push_text("done after two rescues");

    const auto events = wait_for_done(h.loop, [&] {
        h.loop.submit("latest user request");
    });

    EXPECT_EQ(h.provider->turn_count(), 8);
    EXPECT_TRUE(request_contains(h.loop.messages(), "done after two rescues"));
    EXPECT_FALSE(has_error_event(events));
    EXPECT_EQ(count_message_events(events, "system", "第 1 次收缩"), 2)
        << "两次撞墙各自从头开始一轮兜底";
}

// 触发场景:网关的限流报文「您的请求频率已达到限制 (100次/10分钟)」以终止性错误
// 到达 AgentLoop(provider 已放弃重试,或旧版 provider 对非 429 码不重试)。
// 期望行为:它不是上下文超限 —— 不进 PA 兜底(没有「先原样重发」「次收缩」通知)、
// 不给预算学习器记账(压缩阈值不变)、历史一条不动;按普通错误结束回合。
// 回归背景(2026-10-08 反馈):内网限速后用户看到会话一直在压缩,怀疑限流被当成
// 了超限。这条用例钉住:限流报文永远走不进压缩 / 收缩 / 学习三条路。
TEST(AgentLoopPaRescue, RateLimitErrorNeverEntersRescueOrBudgetLearner) {
    RescueWaitGuard wait_guard;
    RescueHarness h("pa_rescue_rate_limit_not_overflow");
    add_history(h.loop, 3);
    acecode::ProviderErrorInfo rate_limited;
    rate_limited.kind = acecode::ProviderErrorKind::Http;
    rate_limited.status_code = 429;
    rate_limited.display_message = "HTTP 429 from openai model aicoder-pro";
    // 线上实测报文,一字不改(SSE 形式,带 data: 前缀)。
    rate_limited.raw_body =
        "data:{\"object\":\"error\",\"message\":\"您的请求频率已达到限制 (100次/10分钟)。"
        "当前已使用: 100次。\",\"type\":\"BadRequestError\",\"code\":429}";
    rate_limited.retryable = false;
    h.provider->push_error(rate_limited);
    h.provider->push_text("never reached in this turn");

    const auto events = wait_for_done(h.loop, [&] {
        h.loop.submit("latest user request");
    });

    EXPECT_EQ(h.provider->turn_count(), 1) << "限流不是超限,不该触发任何重发";
    EXPECT_TRUE(has_error_event(events));
    EXPECT_FALSE(has_system_event(events, "先原样重发"));
    EXPECT_FALSE(has_system_event(events, "次收缩"));
    EXPECT_FALSE(has_system_event(events, "压缩阈值下调"));
    EXPECT_FALSE(acecode::pa::context_budget().has_observation(
        h.provider->name(), h.provider->model()))
        << "限流报文不能进上下文预算学习器";
    EXPECT_TRUE(request_contains(h.loop.messages(), "old assistant 0"));
    EXPECT_TRUE(request_contains(h.loop.messages(), "old user 0"));
}

// Use a long real wait so passing requires cancellation, not an elapsed timer.
TEST(AgentLoopPaRescue, InterruptTurnWakesRescueWaitPromptly) {
    RescueWaitGuard wait_guard;
    acecode::pa::set_rescue_wait_scale_for_test(300.0);
    std::mutex mu;
    std::condition_variable cv;
    bool waiting = false;
    int done_count = 0;
    RescueHarness h("pa_rescue_interrupt_wait");
    h.push_pa_errors(1);
    h.provider->push_text("follow-up response");
    acecode::AgentCallbacks callbacks;
    callbacks.on_model_retry = [&](const acecode::ProviderErrorInfo& info) {
        std::lock_guard<std::mutex> lock(mu);
        waiting = info.retry_delay_ms >= 600000;
        cv.notify_all();
    };
    h.loop.set_callbacks(std::move(callbacks));
    const auto subscription = h.loop.events().subscribe([&](const acecode::SessionEvent& event) {
        if (event.kind != acecode::SessionEventKind::Done) return;
        std::lock_guard<std::mutex> lock(mu);
        ++done_count;
        cv.notify_all();
    });
    h.loop.submit("start a turn");
    bool entered_wait = false;
    {
        std::unique_lock<std::mutex> lock(mu);
        entered_wait = cv.wait_for(lock, 10s, [&] { return waiting; });
    }
    EXPECT_TRUE(entered_wait);
    if (entered_wait) {
        acecode::UserInput follow_up;
        follow_up.text = "interrupt and follow up";
        const auto result = h.loop.interrupt_turn(h.loop.active_turn_id(), follow_up);
        EXPECT_TRUE(result.accepted());
        std::unique_lock<std::mutex> lock(mu);
        EXPECT_TRUE(cv.wait_for(lock, 2s, [&] { return done_count == 2; }));
    }
    h.loop.shutdown();
    h.loop.events().unsubscribe(subscription);
    EXPECT_EQ(done_count, 2);
}
