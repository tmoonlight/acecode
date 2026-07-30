// 端到端测试 AgentLoop 终止协议(openspec/changes/align-loop-with-hermes):
//   (a) text-only 响应直接结束 loop,无条件
//   (b) turn 1 调用 task_complete → 1 轮退出,UI 渲染 Done 摘要
//   (c) 长链工具调用 → 命中 max_iterations 硬上限
//   (c2) 默认 max_iterations=0 → 不因 50 轮默认值提前停止
//   (d) AskUserQuestion 不是终止器 — 模型应继续下一轮(tool_result 走回模型)
//   (e) 用户 abort → 立刻退出,发 [Interrupted] 系统消息
//
// 关键机制:
// - 用 StubLlmProvider 脚本化每轮 LLM 响应
// - 用 on_busy_changed(false) + cv 同步测试主线程
// - 用 on_message 收集所有消息流,断言数量和内容
//
// AgentLoop 的 worker_thread 会在构造时启动,在析构(shutdown)时 join。
// 测试每个用例构造一个独立的 AgentLoop 实例,用 RAII 确保清理。

#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "config/config.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_registry.hpp"
#include "memory/memory_types.hpp"
#include "project_instructions/instructions_loader.hpp"
#include "stub_provider.hpp"
#include "tool/task_complete_tool.hpp"
#include "tool/tool_executor.hpp"
#include "permissions.hpp"
#include "provider/llm_provider.hpp"
#include "provider/retry_policy.hpp"
#include "session/turn_timing.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using acecode::AgentLoop;
using acecode::AgentCallbacks;
using acecode::ChatMessage;
using acecode::PermissionManager;
using acecode::PermissionResult;
using acecode::ProviderErrorInfo;
using acecode::ProviderErrorKind;
using acecode::kProviderRetryMaxDelayMs;
using acecode::ToolDef;
using acecode::ToolExecutor;
using acecode::ToolImpl;
using acecode::ToolResult;
using acecode::ToolSource;
using acecode::UserInput;
using acecode_test::ScriptedResponse;
using acecode_test::StubLlmProvider;

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr const char* kHomeEnvName = "USERPROFILE";
#else
constexpr const char* kHomeEnvName = "HOME";
#endif

void set_env_var(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs << content;
}

class TempHomeGuard {
public:
    explicit TempHomeGuard(std::string name) {
        const char* e = std::getenv(kHomeEnvName);
        prev_home_ = e ? e : "";
        root_ = fs::temp_directory_path() / fs::path(std::move(name));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_);
        set_env_var(kHomeEnvName, root_.string());
    }

    ~TempHomeGuard() {
        set_env_var(kHomeEnvName, prev_home_);
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    const fs::path& root() const { return root_; }

private:
    fs::path root_;
    std::string prev_home_;
};

// 一个零副作用的占位"noop"工具,用于让长链工具调用走通(测 max_iterations 用)。
ToolImpl create_noop_tool() {
    ToolDef def;
    def.name = "noop";
    def.description = "Stub tool for agent-loop tests; returns success immediately.";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object()}
    };
    ToolImpl impl;
    impl.definition = def;
    impl.execute = [](const std::string&, const acecode::ToolContext&) {
        return ToolResult{"ok", true};
    };
    impl.is_read_only = true;  // 避免触发 permission 确认
    impl.source = ToolSource::Builtin;
    return impl;
}

// Fixture:封装 AgentLoop + stub + 消息收集器 + 完成同步。
class AgentLoopHarness {
public:
    explicit AgentLoopHarness(std::string cwd = ".") {
        tools_.register_tool(create_noop_tool());
        tools_.register_tool(acecode::create_task_complete_tool());

        AgentCallbacks cb;
        cb.on_message = [this](const std::string& role,
                               const std::string& content, bool is_tool) {
            std::lock_guard<std::mutex> lk(msg_mu_);
            messages_.push_back({role, content, is_tool});
        };
        cb.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            is_busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        cb.on_turn_finished = [this](const std::string& status) {
            std::lock_guard<std::mutex> lk(outcome_mu_);
            turn_outcomes_.push_back(status);
        };
        cb.on_tool_confirm = [](const std::string&, const std::string&) {
            return PermissionResult::Allow;
        };
        cb.on_delta = [this](const std::string& token) {
            std::lock_guard<std::mutex> lk(msg_mu_);
            live_stream_ += token;
        };
        cb.on_stream_retry_reset = [this]() {
            std::lock_guard<std::mutex> lk(msg_mu_);
            live_stream_.clear();
            ++stream_retry_resets_;
        };
        cb.on_model_retry = [this](const ProviderErrorInfo& info) {
            {
                std::lock_guard<std::mutex> lk(retry_mu_);
                retry_infos_.push_back(info);
            }
            retry_cv_.notify_all();
        };
        cb.on_model_retry_resume = [this]() {
            std::lock_guard<std::mutex> lk(retry_mu_);
            ++retry_resumes_;
        };

        auto provider_accessor =
            [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; };

        loop_ = std::make_unique<AgentLoop>(
            provider_accessor, tools_, cb, /*cwd=*/std::move(cwd), perms_);
        event_sub_ = loop_->events().subscribe(
            [this](const acecode::SessionEvent& event) {
                {
                    std::lock_guard<std::mutex> lk(event_mu_);
                    events_.push_back(event);
                }
                event_cv_.notify_all();
            });
    }

    ~AgentLoopHarness() {
        // on_busy_changed(false) is intentionally emitted before the worker
        // publishes its terminal events. Join first so those callbacks cannot
        // race with destruction of the mutexes/vectors they capture.
        if (loop_) {
            loop_->shutdown();
        }
        if (loop_ && event_sub_ != 0) {
            loop_->events().unsubscribe(event_sub_);
        }
    }

    void set_config(acecode::AgentLoopConfig cfg) {
        loop_->set_agent_loop_config(cfg);
    }

    void set_no_model_prompt(std::string prompt) {
        loop_->set_no_model_config_prompt(std::move(prompt));
    }

    void clear_provider() { provider_.reset(); }

    void set_stub_latency_ms(int ms) { provider_->set_latency_ms(ms); }

    void push_text(std::string s) { provider_->push_text(std::move(s)); }
    void push_provider_error(ProviderErrorInfo error,
                             bool after_payload = false,
                             std::string text = {},
                             std::vector<acecode::ToolCall> tool_calls = {}) {
        provider_->push_error(std::move(error),
                              after_payload,
                              std::move(text),
                              std::move(tool_calls));
    }
    void push_tool_call(std::string name, std::string args, std::string id = "c1") {
        provider_->push_tool_call(std::move(name), std::move(args), std::move(id));
    }
    void push_task_complete(std::string summary, std::string id = "c-done") {
        nlohmann::json args = {{"summary", std::move(summary)}};
        provider_->push_tool_call("task_complete", args.dump(), std::move(id));
    }
    void push_events(std::vector<acecode::StreamEvent> events) {
        provider_->push_events(std::move(events));
    }
    void push_retry_wait(ProviderErrorInfo info) {
        provider_->push_retry_wait(std::move(info));
    }

    // 发消息并阻塞直到 on_busy_changed(false)。返回 false 代表超时(测试失败信号)。
    bool submit_and_wait(const std::string& msg,
                        std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        submit_without_wait(msg);
        return wait_until_idle(timeout);
    }

    void submit_without_wait(const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            is_busy_ = true;  // 认定 submit 前就进入 busy 状态;on_busy_changed 会先升后降
        }
        loop_->submit(msg);
    }

    bool wait_until_idle(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !is_busy_; });
    }

    bool wait_for_retry_count(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        std::unique_lock<std::mutex> lk(retry_mu_);
        return retry_cv_.wait_for(lk, timeout, [this, count] {
            return retry_infos_.size() >= count;
        });
    }

    bool submit_input_and_wait(const UserInput& input,
                        std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            is_busy_ = true;
        }
        loop_->submit(input);
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !is_busy_; });
    }

    void abort() { loop_->abort(); }

    int turn_count() const { return provider_ ? provider_->turn_count() : 0; }

    std::vector<ChatMessage> request_messages_for_turn(int zero_based_index) const {
        return provider_->messages_for_turn(zero_based_index);
    }

    std::vector<ToolDef> request_tools_for_turn(int zero_based_index) const {
        return provider_->tools_for_turn(zero_based_index);
    }

    std::vector<ChatMessage> persisted_messages() const {
        return loop_->messages();
    }

    void set_memory_context(const acecode::MemoryRegistry* registry,
                            const acecode::MemoryConfig* cfg) {
        loop_->set_memory_registry(registry);
        loop_->set_memory_config(cfg);
    }

    void set_project_instructions_config(const acecode::ProjectInstructionsConfig* cfg) {
        loop_->set_project_instructions_config(cfg);
    }

    struct Msg {
        std::string role;
        std::string content;
        bool is_tool = false;
    };

    std::vector<Msg> snapshot_messages() {
        std::lock_guard<std::mutex> lk(msg_mu_);
        return messages_;
    }

    int count_by_role(const std::string& role) {
        std::lock_guard<std::mutex> lk(msg_mu_);
        int n = 0;
        for (const auto& m : messages_) if (m.role == role) ++n;
        return n;
    }

    std::string live_stream() {
        std::lock_guard<std::mutex> lk(msg_mu_);
        return live_stream_;
    }

    int stream_retry_resets() {
        std::lock_guard<std::mutex> lk(msg_mu_);
        return stream_retry_resets_;
    }

    int retry_resumes() {
        std::lock_guard<std::mutex> lk(retry_mu_);
        return retry_resumes_;
    }

    std::vector<acecode::SessionEvent> snapshot_events() {
        std::lock_guard<std::mutex> lk(event_mu_);
        return events_;
    }

    std::string last_turn_outcome() {
        std::lock_guard<std::mutex> lk(outcome_mu_);
        return turn_outcomes_.empty() ? std::string{} : turn_outcomes_.back();
    }

    std::string last_terminal_busy_outcome() {
        std::size_t expected_terminal_events = 0;
        {
            std::lock_guard<std::mutex> outcome_lk(outcome_mu_);
            expected_terminal_events = turn_outcomes_.size();
        }
        std::unique_lock<std::mutex> lk(event_mu_);
        event_cv_.wait_for(lk, std::chrono::seconds(1), [this, expected_terminal_events] {
            return static_cast<std::size_t>(std::count_if(
                       events_.begin(), events_.end(), [](const auto& event) {
                           return event.kind == acecode::SessionEventKind::BusyChanged &&
                                  event.payload.is_object() &&
                                  !event.payload.value("busy", true);
                       })) >= expected_terminal_events;
        });
        for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
            if (it->kind != acecode::SessionEventKind::BusyChanged ||
                !it->payload.is_object() ||
                it->payload.value("busy", true)) {
                continue;
            }
            return it->payload.value("outcome", std::string{});
        }
        return {};
    }

    // align-loop-with-hermes:loop 不再注入 nudge;此 helper 仅作为防回归断言,
    // 任何包含 [acecode:auto-continue] 前缀的 user 消息都说明回归了 nudge 路径。
    int count_nudges() {
        std::lock_guard<std::mutex> lk(msg_mu_);
        int n = 0;
        for (const auto& m : messages_) {
            if (m.role == "user" &&
                m.content.find("[acecode:auto-continue]") != std::string::npos) {
                ++n;
            }
        }
        return n;
    }

    // 找到最后一条 role=system 的消息(诊断停机原因)。
    std::string last_system_message() {
        std::lock_guard<std::mutex> lk(msg_mu_);
        for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
            if (it->role == "system") return it->content;
        }
        return {};
    }

private:
    std::shared_ptr<StubLlmProvider> provider_ = std::make_shared<StubLlmProvider>();
    ToolExecutor tools_;
    PermissionManager perms_;
    std::unique_ptr<AgentLoop> loop_;

    std::mutex msg_mu_;
    std::vector<Msg> messages_;
    std::string live_stream_;
    int stream_retry_resets_ = 0;

    std::mutex retry_mu_;
    std::condition_variable retry_cv_;
    std::vector<ProviderErrorInfo> retry_infos_;
    int retry_resumes_ = 0;

    std::mutex outcome_mu_;
    std::vector<std::string> turn_outcomes_;

    acecode::EventDispatcher::SubscriptionId event_sub_ = 0;
    std::mutex event_mu_;
    std::condition_variable event_cv_;
    std::vector<acecode::SessionEvent> events_;

    std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool is_busy_ = false;
};

} // namespace

ProviderErrorInfo make_stub_provider_error(std::string display = "HTTP 500 from stub") {
    ProviderErrorInfo error;
    error.kind = ProviderErrorKind::Http;
    error.status_code = 500;
    error.provider = "stub";
    error.model = "stub-1";
    error.display_message = std::move(display);
    error.raw_body = R"({"error":"boom"})";
    error.body_is_json = true;
    error.pretty_json = "{\n  \"error\": \"boom\"\n}";
    error.retryable = true;
    return error;
}

std::vector<acecode::TurnTimingRecord> turn_timings_from(
    const std::vector<ChatMessage>& messages) {
    std::vector<acecode::TurnTimingRecord> out;
    for (const auto& msg : messages) {
        if (!msg.metadata.is_object()) continue;
        auto timing = acecode::decode_turn_timing(msg.metadata.value("turn_timing", nlohmann::json{}));
        if (timing.has_value()) {
            EXPECT_TRUE(msg.metadata.value("transcript_only", false));
            out.push_back(*timing);
        }
    }
    return out;
}

TEST(AgentLoopTurnTiming, CompletedTurnWritesTranscriptOnlyTiming) {
    AgentLoopHarness h;
    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("work"));

    const auto messages = h.persisted_messages();
    auto timings = turn_timings_from(messages);
    ASSERT_EQ(timings.size(), 1u);
    ASSERT_GE(messages.size(), 3u);
    EXPECT_EQ(messages.front().role, "user");
    EXPECT_EQ(timings[0].user_message_uuid, messages.front().uuid);
    EXPECT_EQ(timings[0].status, "completed");
    EXPECT_GE(timings[0].duration_ms, 0);
    EXPECT_GE(timings[0].completed_at_ms, timings[0].started_at_ms);
}

TEST(AgentLoopTurnTiming, AbortedTurnWritesOneAbortedTiming) {
    AgentLoopHarness h;
    h.set_stub_latency_ms(200);
    h.push_tool_call("noop", "{}", "c1");

    std::thread aborter([&h] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        h.abort();
    });
    ASSERT_TRUE(h.submit_and_wait("abort me", std::chrono::seconds(10)));
    aborter.join();

    auto timings = turn_timings_from(h.persisted_messages());
    ASSERT_EQ(timings.size(), 1u);
    EXPECT_EQ(timings[0].status, "aborted");
}

TEST(AgentLoopTurnTiming, ProviderErrorWritesOneErrorTiming) {
    AgentLoopHarness h;
    h.push_provider_error(make_stub_provider_error("provider failed"));

    ASSERT_TRUE(h.submit_and_wait("fail"));

    auto timings = turn_timings_from(h.persisted_messages());
    ASSERT_EQ(timings.size(), 1u);
    EXPECT_EQ(timings[0].status, "error");
}

TEST(AgentLoopTurnTiming, HiddenContextInputDoesNotCreateTiming) {
    AgentLoopHarness h;
    h.push_text("hidden answer");
    UserInput input;
    input.text = "hidden goal context";
    input.metadata = nlohmann::json{{"hidden_goal_context", true}};

    ASSERT_TRUE(h.submit_input_and_wait(input));

    auto timings = turn_timings_from(h.persisted_messages());
    EXPECT_TRUE(timings.empty());
}

TEST(AgentLoopTurnTiming, TranscriptOnlyTimingDoesNotEnterNextProviderRequest) {
    AgentLoopHarness h;
    h.push_text("first done");
    ASSERT_TRUE(h.submit_and_wait("first"));
    ASSERT_EQ(turn_timings_from(h.persisted_messages()).size(), 1u);

    h.push_text("second done");
    ASSERT_TRUE(h.submit_and_wait("second"));

    const auto second_request = h.request_messages_for_turn(1);
    ASSERT_FALSE(second_request.empty());
    for (const auto& msg : second_request) {
        if (msg.metadata.is_object()) {
            EXPECT_FALSE(msg.metadata.value("transcript_only", false));
            EXPECT_FALSE(msg.metadata.contains("turn_timing"));
        }
    }
}

// 场景:provider 在同一请求内部对临时网络故障重试时,AgentLoop 必须清掉
// 失败连接的全部 provisional 状态,暴露非 transcript 的等待/恢复进度,
// 最终只持久化成功重试的输出。
TEST(AgentLoopTermination, TransientRetryResetsProvisionalStateAndReportsProgress) {
    AgentLoopHarness h;

    ProviderErrorInfo network = make_stub_provider_error("connection lost");
    network.kind = ProviderErrorKind::Network;
    network.status_code = 0;
    network.retryable = true;
    network.retry_attempt = 3;
    network.retry_max_attempts = -1;
    network.retry_delay_ms = 4000;

    acecode::StreamEvent partial;
    partial.type = acecode::StreamEventType::Delta;
    partial.content = "partial";

    acecode::StreamEvent reasoning;
    reasoning.type = acecode::StreamEventType::ReasoningDelta;
    reasoning.content = "provisional reasoning";

    acecode::StreamEvent usage;
    usage.type = acecode::StreamEventType::Usage;
    usage.usage.has_data = true;
    usage.usage.prompt_tokens = 100;
    usage.usage.completion_tokens = 50;
    usage.usage.total_tokens = 150;

    acecode::StreamEvent provisional_tool;
    provisional_tool.type = acecode::StreamEventType::ToolCall;
    provisional_tool.tool_call.id = "provisional-call";
    provisional_tool.tool_call.function_name = "noop";
    provisional_tool.tool_call.function_arguments = "{}";

    acecode::StreamEvent retry;
    retry.type = acecode::StreamEventType::Retry;
    retry.provider_error = network;
    retry.error = network.display_message;

    acecode::StreamEvent resumed;
    resumed.type = acecode::StreamEventType::RetryResume;
    resumed.provider_error = network;

    acecode::StreamEvent final_delta;
    final_delta.type = acecode::StreamEventType::Delta;
    final_delta.content = "final";

    acecode::StreamEvent final_usage;
    final_usage.type = acecode::StreamEventType::Usage;
    final_usage.usage.has_data = true;
    final_usage.usage.prompt_tokens = 7;
    final_usage.usage.completion_tokens = 3;
    final_usage.usage.total_tokens = 10;

    acecode::StreamEvent done;
    done.type = acecode::StreamEventType::Done;

    h.push_events({
        partial,
        reasoning,
        usage,
        provisional_tool,
        retry,
        resumed,
        final_delta,
        final_usage,
        done,
    });

    ASSERT_TRUE(h.submit_and_wait("run"));
    EXPECT_EQ(h.stream_retry_resets(), 1);
    EXPECT_EQ(h.retry_resumes(), 1);
    EXPECT_EQ(h.live_stream(), "final");

    auto persisted = h.persisted_messages();
    int assistant_count = 0;
    std::string assistant_content;
    for (const auto& msg : persisted) {
        if (msg.role == "assistant") {
            ++assistant_count;
            assistant_content = msg.content;
        }
    }
    EXPECT_EQ(assistant_count, 1);
    EXPECT_EQ(assistant_content, "final");
    EXPECT_EQ(assistant_content.find("partial"), std::string::npos);
    for (const auto& msg : persisted) {
        EXPECT_NE(msg.tool_call_id, "provisional-call");
        EXPECT_EQ(
            msg.tool_calls.dump().find("provisional-call"),
            std::string::npos);
    }

    bool saw_retry_progress = false;
    bool saw_resume_progress = false;
    bool saw_transcript_reset = false;
    int usage_event_count = 0;
    for (const auto& event : h.snapshot_events()) {
        if (event.kind == acecode::SessionEventKind::Usage) {
            ++usage_event_count;
            EXPECT_EQ(event.payload.value("total_tokens", 0), 10);
        }
        if (event.kind == acecode::SessionEventKind::TranscriptReplace) {
            saw_transcript_reset = true;
        }
        if (event.kind != acecode::SessionEventKind::AgentProgress ||
            !event.payload.is_object()) {
            continue;
        }
        const std::string phase =
            event.payload.value("phase", std::string{});
        if (phase == "model_retry") {
            saw_retry_progress = true;
            EXPECT_EQ(event.payload.value("retry_attempt", 0), 3);
            EXPECT_EQ(event.payload.value("retry_delay_ms", 0), 4000);
            EXPECT_EQ(event.payload.value("retry_max_attempts", 0), -1);
            EXPECT_GT(event.payload.value("retry_at_ms", 0LL), 0);
        } else if (phase == "model_waiting" &&
                   event.payload.contains("retry_attempt")) {
            saw_resume_progress = true;
        }
    }
    EXPECT_TRUE(saw_retry_progress);
    EXPECT_TRUE(saw_resume_progress);
    EXPECT_TRUE(saw_transcript_reset);
    EXPECT_EQ(usage_event_count, 1);

    for (const auto& message : h.snapshot_messages()) {
        EXPECT_EQ(
            message.content.find("网络暂时不可用"),
            std::string::npos);
    }
}

// 场景:任务已进入 20 分钟封顶等待时,stop 必须通知 active provider 的
// condition variable,立即结束,而不是等到下一次定时唤醒。
TEST(AgentLoopTermination, AbortWakesTwentyMinuteRetryWaitPromptly) {
    AgentLoopHarness h;
    ProviderErrorInfo network = make_stub_provider_error("offline");
    network.kind = ProviderErrorKind::Network;
    network.status_code = 0;
    network.retry_attempt = 20;
    network.retry_max_attempts = -1;
    network.retry_delay_ms = static_cast<int>(kProviderRetryMaxDelayMs);
    h.push_retry_wait(network);

    h.submit_without_wait("keep running");
    ASSERT_TRUE(h.wait_for_retry_count(1));

    const auto started = std::chrono::steady_clock::now();
    h.abort();
    ASSERT_TRUE(h.wait_until_idle(std::chrono::seconds(2)));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(1));
    EXPECT_EQ(h.turn_count(), 1);
    EXPECT_EQ(h.count_by_role("error"), 0);
    EXPECT_EQ(h.last_system_message(), "[Interrupted]");
    EXPECT_EQ(h.last_turn_outcome(), "aborted");
}

// 场景 (a):text-only 响应直接结束 loop。chit-chat 与 mid-task hedge 都走这条路径。
TEST(AgentLoopTermination, TextOnlyEndsTurnUnconditionally) {
    AgentLoopHarness h;
    h.push_text("你好!有什么可以帮你的?");

    ASSERT_TRUE(h.submit_and_wait("你好"));
    EXPECT_EQ(h.turn_count(), 1);
    EXPECT_EQ(h.count_nudges(), 0);  // 防回归:绝不该出现 nudge
    EXPECT_EQ(h.last_system_message().find("Agent loop stopped"),
              std::string::npos);  // 正常退出,无 cap 消息
}

// 场景:同一回合内的多次采样请求必须共享逐字节相同的前缀,否则 provider
// 的 prompt cache 每轮都从注入点被截断,整条工具调用尾巴全价重算。
//
// 回归背景:注入到最后一条真实 user 消息之前的可变上下文块曾经拼进一个
// 秒级时间戳,于是每一轮工具往返都换一份内容,缓存前缀在此断开。日期与
// cwd 现在留在静态 system prompt 里(普通采样迭代间稳定),可变块只随内容变化。
TEST(AgentLoopTermination, RequestPrefixIsByteStableAcrossIterationsInATurn) {
    AgentLoopHarness h;
    h.push_tool_call("noop", "{}", "c1");
    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("run the tool"));
    ASSERT_EQ(h.turn_count(), 2);

    const auto first = h.request_messages_for_turn(0);
    const auto second = h.request_messages_for_turn(1);
    // A clean CTest working directory has only system + user messages, while
    // running from the repository may also load project instructions.
    ASSERT_GE(first.size(), 2u);
    ASSERT_GT(second.size(), first.size());

    // 第二次请求只应在第一次的末尾追加(assistant 工具调用 + 工具结果),
    // 前面每一条消息(system prompt、历史、注入的可变上下文)必须一致。
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].role, second[i].role) << "role mismatch at " << i;
        EXPECT_EQ(first[i].content, second[i].content)
            << "prompt prefix changed at message " << i
            << " - this breaks provider prompt caching for the whole turn";
    }

    // 静态 system prompt 携带低频变化的日期与 cwd,时分秒不进 prompt。
    EXPECT_EQ(first.front().role, "system");
    EXPECT_NE(first.front().content.find("- Today's date: "), std::string::npos);
    EXPECT_NE(first.front().content.find("- Working directory: "), std::string::npos);
    EXPECT_EQ(first.front().content.find("[当前环境状态]"), std::string::npos);
}

// 场景:注入的可变上下文只进 API 消息,不落会话历史。
TEST(AgentLoopTermination, InjectedContextIsApiOnlyAtHandoffBoundary) {
    AgentLoopHarness h;
    h.push_text("ok");

    ASSERT_TRUE(h.submit_and_wait("what time is it?"));
    ASSERT_EQ(h.turn_count(), 1);

    auto request = h.request_messages_for_turn(0);
    ASSERT_GE(request.size(), 2u);
    EXPECT_EQ(request.front().role, "system");
    EXPECT_EQ(request.back().role, "user");
    EXPECT_EQ(request.back().content, "what time is it?");

    auto persisted = h.persisted_messages();
    ASSERT_GE(persisted.size(), 2u);
    EXPECT_EQ(persisted[0].role, "user");
    EXPECT_EQ(persisted[0].content, "what time is it?");
    for (const auto& msg : persisted) {
        EXPECT_EQ(msg.content.find("<system-reminder>"), std::string::npos);
    }
}

// 场景:Project Instructions / User Memory 从静态 system prompt 移到
// provider-facing session context,且不进入持久历史。
TEST(AgentLoopTermination, SessionContextIsApiOnlyAndStaticPromptStaysClean) {
    TempHomeGuard home("acecode-agentloop-context");
    fs::path repo = home.root() / "repo";
    write_file(repo / "AGENT.md", "# repo rules\nuse goroutines\n");

    fs::create_directories(acecode::get_memory_dir());
    acecode::MemoryRegistry memory;
    memory.scan();
    std::string err;
    ASSERT_TRUE(memory.upsert("user_profile", acecode::MemoryType::User,
                              "senior Go dev", "10y Go\n",
                              acecode::MemoryWriteMode::Create, err).has_value())
        << err;

    acecode::MemoryConfig memory_cfg;
    acecode::ProjectInstructionsConfig project_cfg;

    AgentLoopHarness h(repo.string());
    h.set_memory_context(&memory, &memory_cfg);
    h.set_project_instructions_config(&project_cfg);
    h.push_text("ok");

    ASSERT_TRUE(h.submit_and_wait("what should I do?"));
    ASSERT_EQ(h.turn_count(), 1);

    auto request = h.request_messages_for_turn(0);
    ASSERT_GE(request.size(), 3u);
    ASSERT_EQ(request.front().role, "system");
    EXPECT_EQ(request.front().content.find("# Project Instructions"), std::string::npos);
    EXPECT_EQ(request.front().content.find("# User Memory"), std::string::npos);
    EXPECT_EQ(request.front().content.find("use goroutines"), std::string::npos);
    EXPECT_EQ(request.front().content.find("user_profile.md"), std::string::npos);

    bool saw_project = false;
    bool saw_memory = false;
    for (const auto& msg : request) {
        if (msg.content.find("# Project Instructions") != std::string::npos &&
            msg.content.find("use goroutines") != std::string::npos) {
            saw_project = true;
        }
        if (msg.content.find("# User Memory") != std::string::npos &&
            msg.content.find("user_profile.md") != std::string::npos) {
            saw_memory = true;
        }
    }
    EXPECT_TRUE(saw_project);
    EXPECT_TRUE(saw_memory);
    EXPECT_EQ(request.back().content, "what should I do?");
    ASSERT_GE(request.size(), 2u);
    // 可变上下文紧贴在最后一条真实 user 消息之前。
    const auto& injected = request[request.size() - 2];
    EXPECT_EQ(injected.role, "user");
    EXPECT_NE(injected.content.find("<system-reminder>"), std::string::npos);
    EXPECT_EQ(injected.content.find("[用户输入]"), std::string::npos);

    auto persisted = h.persisted_messages();
    for (const auto& msg : persisted) {
        EXPECT_EQ(msg.content.find("# Project Instructions"), std::string::npos);
        EXPECT_EQ(msg.content.find("# User Memory"), std::string::npos);
        EXPECT_EQ(msg.content.find("<system-reminder>"), std::string::npos);
    }
}

// 场景:项目文件和 memory mid-session 变化时,provider context 更新,
// 但静态 system prompt 字节不变。
TEST(AgentLoopTermination, MutableContextChangesDoNotChangeStaticSystemPrompt) {
    TempHomeGuard home("acecode-agentloop-context-edit");
    fs::path repo = home.root() / "repo";
    write_file(repo / "AGENT.md", "before rule\n");

    fs::create_directories(acecode::get_memory_dir());
    acecode::MemoryRegistry memory;
    memory.scan();
    std::string err;
    ASSERT_TRUE(memory.upsert("first_memory", acecode::MemoryType::User,
                              "first memory", "before\n",
                              acecode::MemoryWriteMode::Create, err).has_value())
        << err;

    acecode::MemoryConfig memory_cfg;
    acecode::ProjectInstructionsConfig project_cfg;

    AgentLoopHarness h(repo.string());
    h.set_memory_context(&memory, &memory_cfg);
    h.set_project_instructions_config(&project_cfg);
    h.push_text("first ok");
    ASSERT_TRUE(h.submit_and_wait("first"));

    write_file(repo / "AGENT.md", "after rule\n");
    ASSERT_TRUE(memory.upsert("second_memory", acecode::MemoryType::User,
                              "second memory", "after\n",
                              acecode::MemoryWriteMode::Create, err).has_value())
        << err;

    h.push_text("second ok");
    ASSERT_TRUE(h.submit_and_wait("second"));

    auto first_request = h.request_messages_for_turn(0);
    auto second_request = h.request_messages_for_turn(1);
    ASSERT_FALSE(first_request.empty());
    ASSERT_FALSE(second_request.empty());
    EXPECT_EQ(first_request.front().role, "system");
    EXPECT_EQ(second_request.front().role, "system");
    EXPECT_EQ(first_request.front().content, second_request.front().content);

    auto contains = [](const std::vector<ChatMessage>& messages,
                       const std::string& needle) {
        for (const auto& msg : messages) {
            if (msg.content.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    EXPECT_TRUE(contains(first_request, "before rule"));
    EXPECT_FALSE(contains(first_request, "after rule"));
    EXPECT_TRUE(contains(first_request, "first_memory.md"));
    EXPECT_FALSE(contains(first_request, "second_memory.md"));

    EXPECT_TRUE(contains(second_request, "after rule"));
    EXPECT_TRUE(contains(second_request, "second_memory.md"));
}

// 场景 (b):turn 1 就调用 task_complete → 1 轮退出,无 cap 消息
TEST(AgentLoopTermination, TaskCompleteTerminatesImmediately) {
    AgentLoopHarness h;
    h.push_task_complete("done in one turn");

    ASSERT_TRUE(h.submit_and_wait("do something"));
    EXPECT_EQ(h.turn_count(), 1);
    EXPECT_EQ(h.count_nudges(), 0);
    EXPECT_EQ(h.last_system_message().find("Agent loop stopped"),
              std::string::npos);
}

// 场景 (c):max_iterations 硬上限触发
TEST(AgentLoopTermination, MaxIterationsHardCap) {
    AgentLoopHarness h;
    acecode::AgentLoopConfig cfg;
    cfg.max_iterations = 3;
    h.set_config(cfg);

    // 全部 tool-call,保证 loop 不在 text-only 分支提前退出
    for (int i = 0; i < 10; ++i) {
        h.push_tool_call("noop", "{}", "c" + std::to_string(i));
    }

    ASSERT_TRUE(h.submit_and_wait("do it"));
    EXPECT_EQ(h.turn_count(), 3);
    EXPECT_NE(h.last_system_message().find("max_iterations"),
              std::string::npos);
    EXPECT_EQ(h.last_turn_outcome(), "error");
    EXPECT_EQ(h.last_terminal_busy_outcome(), "error");
}

// 场景 (c2):默认 max_iterations=0 表示无限制,不会按旧默认 50 轮停止。
TEST(AgentLoopTermination, DefaultMaxIterationsIsUnlimited) {
    AgentLoopHarness h;

    for (int i = 0; i < 55; ++i) {
        h.push_tool_call("noop", "{}", "c" + std::to_string(i));
    }
    h.push_task_complete("done after old default");

    ASSERT_TRUE(h.submit_and_wait("do it", std::chrono::seconds(10)));
    EXPECT_EQ(h.turn_count(), 56);
    EXPECT_EQ(h.last_system_message().find("max_iterations"),
              std::string::npos);
}

// 场景 (d):AskUserQuestion 不是终止器 —— tool_result 回模型后 loop 应该继续。
// 模拟流程:模型第 1 轮调 AskUserQuestion → tool 未注册返回 "Unknown tool"
// → 第 2 轮模型看到 tool_result,调 task_complete → 退出。
// 关键断言:turn_count == 2(loop 没在 AskUserQuestion 这轮就结束)。
TEST(AgentLoopTermination, AskUserQuestionDoesNotTerminate) {
    AgentLoopHarness h;
    h.push_tool_call("AskUserQuestion", R"({"questions":[]})", "ask-1");
    h.push_task_complete("acknowledged");

    ASSERT_TRUE(h.submit_and_wait("do it"));
    EXPECT_EQ(h.turn_count(), 2);
    EXPECT_EQ(h.count_nudges(), 0);
}

// 场景 (e):用户 abort 立刻生效。让 stub 的 chat_stream 阻塞 ~200ms 轮询
// abort_flag,给主线程一个确定性窗口下 abort。
TEST(AgentLoopTermination, UserAbortShortCircuits) {
    AgentLoopHarness h;
    acecode::AgentLoopConfig cfg;
    cfg.max_iterations = 50;
    h.set_config(cfg);
    h.set_stub_latency_ms(200);  // 第一轮 chat_stream 至少停 200ms 轮询 abort

    // 全 tool_call 让 loop 反复转,abort 必须能截停
    for (int i = 0; i < 10; ++i) h.push_tool_call("noop", "{}", "c" + std::to_string(i));

    std::thread t([&h]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        h.abort();
    });
    ASSERT_TRUE(h.submit_and_wait("do it", std::chrono::seconds(10)));
    t.join();

    // Abort 不应触发 max_iterations cap 信息
    const std::string last = h.last_system_message();
    EXPECT_EQ(last.find("max_iterations"), std::string::npos);
    // 也绝不该累积 nudge
    EXPECT_EQ(h.count_nudges(), 0);
    EXPECT_EQ(h.count_by_role("error"), 0);
    EXPECT_EQ(last, "[Interrupted]");
    EXPECT_EQ(h.last_turn_outcome(), "aborted");
    EXPECT_EQ(h.last_terminal_busy_outcome(), "aborted");
}

TEST(AgentLoopTermination, ProviderErrorDoesNotCreateEmptyAssistantAndNextTurnWorks) {
    AgentLoopHarness h;
    h.push_provider_error(make_stub_provider_error());

    ASSERT_TRUE(h.submit_and_wait("first"));
    EXPECT_EQ(h.turn_count(), 1);
    EXPECT_EQ(h.count_by_role("error"), 1);
    EXPECT_EQ(h.count_by_role("assistant"), 0);
    EXPECT_EQ(h.last_turn_outcome(), "error");
    EXPECT_EQ(h.last_terminal_busy_outcome(), "error");

    h.push_text("ok");
    ASSERT_TRUE(h.submit_and_wait("second"));
    EXPECT_EQ(h.turn_count(), 2);
    EXPECT_EQ(h.count_by_role("assistant"), 1);
    EXPECT_EQ(h.last_turn_outcome(), "completed");
    EXPECT_EQ(h.last_terminal_busy_outcome(), "completed");
}

TEST(AgentLoopTermination, NullProviderPromptsUserToConfigureModel) {
    AgentLoopHarness h;
    h.set_no_model_prompt(u8"请先配置大模型服务。TUI 可运行 acecode configure 或使用 /model add 添加模型。");
    h.clear_provider();

    ASSERT_TRUE(h.submit_and_wait("hello"));

    const auto messages = h.snapshot_messages();
    auto it = std::find_if(messages.begin(), messages.end(), [](const auto& msg) {
        return msg.role == "error" &&
               msg.content.find(u8"请先配置大模型服务") != std::string::npos;
    });
    EXPECT_NE(it, messages.end());
    ASSERT_NE(it, messages.end());
    EXPECT_NE(it->content.find("TUI"), std::string::npos);
    EXPECT_EQ(it->content.find(u8"设置 > 模型"), std::string::npos);
    EXPECT_EQ(h.turn_count(), 0);
}

TEST(AgentLoopTermination, ProviderErrorAfterToolCallDoesNotExecuteOrPersistToolCall) {
    AgentLoopHarness h;
    acecode::ToolCall tc;
    tc.id = "call-failed";
    tc.function_name = "noop";
    tc.function_arguments = "{}";
    h.push_provider_error(make_stub_provider_error("stream ended before done"),
                          true,
                          std::string{},
                          {tc});

    ASSERT_TRUE(h.submit_and_wait("use tool"));
    EXPECT_EQ(h.count_by_role("error"), 1);
    EXPECT_EQ(h.count_by_role("assistant"), 0);
    EXPECT_EQ(h.count_by_role("tool_call"), 0);
    EXPECT_EQ(h.count_by_role("tool_result"), 0);
}

TEST(AgentLoopTermination, TimeoutAfterPartialToolCallIsNotReplayedAsOrphan) {
    AgentLoopHarness h;
    acecode::ToolCall tc;
    tc.id = "call-timeout";
    tc.function_name = "noop";
    tc.function_arguments = "{}";
    ProviderErrorInfo timeout = make_stub_provider_error("request timed out");
    timeout.kind = ProviderErrorKind::Timeout;
    timeout.status_code = 200;

    h.push_provider_error(std::move(timeout), true, std::string{}, {tc});

    ASSERT_TRUE(h.submit_and_wait("use tool"));
    EXPECT_EQ(h.count_by_role("error"), 1);
    EXPECT_EQ(h.count_by_role("assistant"), 0);
    EXPECT_EQ(h.count_by_role("tool_call"), 0);
    EXPECT_EQ(h.count_by_role("tool_result"), 0);

    h.push_text("ok");
    ASSERT_TRUE(h.submit_and_wait("next"));

    const auto second_request = h.request_messages_for_turn(1);
    for (const auto& msg : second_request) {
        EXPECT_NE(msg.role, "tool");
        if (msg.role == "assistant") {
            EXPECT_TRUE(msg.tool_calls.is_null() || msg.tool_calls.empty());
        }
    }
}

// ============ 空回复兜底(fix-glm-empty-response-turn-end)============
//
// 回归背景(用户反馈会话 20260703-022813-6f8f,火山引擎 GLM):模型把整个回合
// 的输出 token 预算全部耗在深度思考上(reasoning_content 9932 字符),之后
// HTTP 200 + [DONE] 正常收尾,但 content 与 tool_calls 全空。旧行为把这种
// 「成功但空」的响应当作正常 text-only 回复静默终止回合,用户看到"思考了半天
// 然后什么都没说就停了"(反馈原文:"没有完成任务就停止了")。
// 新行为:空回复触发自动重试 —— 注入 hidden user 提示(hidden_goal_context,
// 进 API 但 TUI/Web 不显示),最多 kMaxEmptyResponseRetries=2 次;耗尽后以显式
// error 结束回合,绝不静默终止。finish_reason 由 Done 事件透传,可能为空
// (部分兼容网关不上报),兜底逻辑不依赖它,只用它增强提示语。

// 构造「仅思考无正文」的空回复事件脚本,贴近火山 GLM 实测形态。
// finish_reason 为空 = 网关未上报(必须也能触发兜底,这是主防线语义)。
static std::vector<acecode::StreamEvent> make_reasoning_only_response(
    const std::string& reasoning, const std::string& finish_reason = {}) {
    acecode::StreamEvent think;
    think.type = acecode::StreamEventType::ReasoningDelta;
    think.content = reasoning;
    acecode::StreamEvent done;
    done.type = acecode::StreamEventType::Done;
    done.finish_reason = finish_reason;
    return {think, done};
}

// 场景:第 1 轮空回复(仅 reasoning,无 finish_reason)→ 注入重试提示 →
// 第 2 轮模型恢复正常文本 → 回合正常完成,无 error。
// 同时验证:空 assistant 消息持久化留档但不 dispatch 到实时流(不出空气泡);
// 注入提示对 API 可见、带 empty_response_retry 标记;不走已移除的
// [acecode:auto-continue] nudge 旧路径(防回归断言)。
TEST(AgentLoopTermination, EmptyResponseInjectsRetryPromptAndRecovers) {
    AgentLoopHarness h;
    h.push_events(make_reasoning_only_response("长篇思考后忘了说话"));
    h.push_text("恢复后的正常回答");

    ASSERT_TRUE(h.submit_and_wait("分析这个项目"));
    EXPECT_EQ(h.turn_count(), 2);
    EXPECT_EQ(h.count_by_role("error"), 0);
    EXPECT_EQ(h.count_nudges(), 0);  // 绝不复活旧 auto-continue 路径

    // 实时流:只有第 2 轮的非空 assistant 被 dispatch(空回复不出气泡)。
    EXPECT_EQ(h.count_by_role("assistant"), 1);

    // 持久历史:空 assistant(带 reasoning)与正常 assistant 都留档。
    const auto persisted = h.persisted_messages();
    int persisted_assistant = 0;
    bool saw_empty_with_reasoning = false;
    bool saw_retry_marker = false;
    for (const auto& msg : persisted) {
        if (msg.role == "assistant") {
            ++persisted_assistant;
            if (msg.content.empty() && !msg.reasoning_content.empty()) {
                saw_empty_with_reasoning = true;
            }
        }
        if (msg.role == "user" && msg.metadata.is_object() &&
            msg.metadata.value("empty_response_retry", false)) {
            saw_retry_marker = true;
            EXPECT_TRUE(msg.metadata.value("hidden_goal_context", false));
        }
    }
    EXPECT_EQ(persisted_assistant, 2);
    EXPECT_TRUE(saw_empty_with_reasoning);
    EXPECT_TRUE(saw_retry_marker);

    // 注入提示必须进第 2 轮 API 请求,模型才可能自我纠正。
    const auto second_request = h.request_messages_for_turn(1);
    bool prompt_in_request = false;
    for (const auto& msg : second_request) {
        if (msg.role == "user" &&
            msg.content.find("[SYSTEM NOTE]") != std::string::npos &&
            msg.content.find("empty") != std::string::npos) {
            prompt_in_request = true;
        }
    }
    EXPECT_TRUE(prompt_in_request);

    // 用户可见的 transcript 系统提示(告知发生了自动重试)。
    const auto messages = h.snapshot_messages();
    bool saw_notice = false;
    for (const auto& m : messages) {
        if (m.role == "system" &&
            m.content.find(u8"[空回复]") != std::string::npos) {
            saw_notice = true;
        }
    }
    EXPECT_TRUE(saw_notice);
}

// 场景:模型连续 3 轮(首轮 + 2 次重试)都返回空回复 → 重试耗尽,回合以显式
// error 结束,turn timing 状态为 "error"。
// 回归断言:旧行为在第 1 轮就静默"正常完成"(反馈用户看到的 bug 表现);
// 新行为必须把失败暴露出来。stub 脚本耗尽后恰好每轮都只发 Done,天然模拟
// 连续空回复。
TEST(AgentLoopTermination, EmptyResponseExhaustsRetriesEndsWithError) {
    AgentLoopHarness h;

    ASSERT_TRUE(h.submit_and_wait("empty"));
    EXPECT_EQ(h.turn_count(), 3);  // 1 首轮 + 2 次重试
    EXPECT_EQ(h.count_by_role("error"), 1);

    // error 文案要说清楚发生了什么(空回复),不能是笼统的失败。
    bool saw_empty_error = false;
    for (const auto& m : h.snapshot_messages()) {
        if (m.role == "error" &&
            m.content.find(u8"空回复") != std::string::npos) {
            saw_empty_error = true;
        }
    }
    EXPECT_TRUE(saw_empty_error);

    // 每一轮的空 assistant 都持久化留档(诊断证据),但实时流零空气泡。
    EXPECT_EQ(h.count_by_role("assistant"), 0);
    int persisted_assistant = 0;
    for (const auto& msg : h.persisted_messages()) {
        if (msg.role == "assistant") ++persisted_assistant;
    }
    EXPECT_EQ(persisted_assistant, 3);

    // turn timing 必须记为 error,不能伪装成 completed。
    auto timings = turn_timings_from(h.persisted_messages());
    ASSERT_EQ(timings.size(), 1u);
    EXPECT_EQ(timings[0].status, "error");
}

// 场景:空回复且 Done 事件带 finish_reason="length"(思考耗尽输出 token 预算,
// 即火山 GLM 反馈会话的推断根因)→ 注入提示与用户提示都应点明「token 上限
// 截断」,引导模型下一轮压缩思考。
TEST(AgentLoopTermination, LengthTruncatedEmptyResponseMentionsTokenLimit) {
    AgentLoopHarness h;
    h.push_events(make_reasoning_only_response("超长思考直到被截断", "length"));
    h.push_text("ok");

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(h.turn_count(), 2);
    EXPECT_EQ(h.count_by_role("error"), 0);

    const auto second_request = h.request_messages_for_turn(1);
    bool prompt_mentions_limit = false;
    for (const auto& msg : second_request) {
        if (msg.role == "user" &&
            msg.content.find("finish_reason=length") != std::string::npos) {
            prompt_mentions_limit = true;
        }
    }
    EXPECT_TRUE(prompt_mentions_limit);

    bool notice_mentions_limit = false;
    for (const auto& m : h.snapshot_messages()) {
        if (m.role == "system" &&
            m.content.find(u8"token 上限截断") != std::string::npos) {
            notice_mentions_limit = true;
        }
    }
    EXPECT_TRUE(notice_mentions_limit);
}

// 场景:非空文本回复但 finish_reason="length"(答案写到一半被截)→ 回合照常
// 结束(不重试 —— 已有部分内容,重发只会浪费且可能重复),但必须给用户一条
// [输出截断] 系统提示,不能假装回复完整。
TEST(AgentLoopTermination, LengthTruncatedNonEmptyTextEndsTurnWithNotice) {
    AgentLoopHarness h;
    acecode::StreamEvent partial_text;
    partial_text.type = acecode::StreamEventType::Delta;
    partial_text.content = "部分回答被截";
    acecode::StreamEvent done;
    done.type = acecode::StreamEventType::Done;
    done.finish_reason = "length";
    h.push_events({partial_text, done});

    ASSERT_TRUE(h.submit_and_wait("answer me"));
    EXPECT_EQ(h.turn_count(), 1);  // 无重试
    EXPECT_EQ(h.count_by_role("error"), 0);
    EXPECT_EQ(h.count_by_role("assistant"), 1);

    bool saw_truncation_notice = false;
    for (const auto& m : h.snapshot_messages()) {
        if (m.role == "system" &&
            m.content.find(u8"[输出截断]") != std::string::npos) {
            saw_truncation_notice = true;
        }
    }
    EXPECT_TRUE(saw_truncation_notice);
}
