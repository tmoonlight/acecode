#include "test_support/agent/agent_loop_fixture.hpp"
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

#include "agent/agent_loop.hpp"
#include "config/config.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_registry.hpp"
#include "memory/memory_service.hpp"
#include "memory/memory_types.hpp"
#include "project_instructions/instructions_loader.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/task_complete_tool.hpp"
#include "tool/tool_executor.hpp"
#include "llm/tool_protocol_names.hpp"
#include "permissions/permissions.hpp"
#include "llm/llm_provider.hpp"
#include "provider/retry_policy.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/session_manager.hpp"
#include "session/tool_result_storage.hpp"
#include "session/turn_net_diff.hpp"
#include "session/turn_timing.hpp"
#include "agent/event_payload/message_payload.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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

ToolImpl create_terminal_session_tool(
    const std::shared_ptr<std::atomic<int>>& executions,
    const std::shared_ptr<std::atomic<int>>& post_turn_actions) {
    ToolImpl impl;
    impl.definition.name = "terminal_session_action";
    impl.definition.description = "Test-only terminal session action.";
    impl.definition.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object()}
    };
    impl.execute = [executions, post_turn_actions](
                       const std::string&, const acecode::ToolContext&) {
        ++*executions;
        ToolResult result{"scheduled", true};
        result.terminate_session_after_turn = true;
        result.post_turn_action = [post_turn_actions] {
            ++*post_turn_actions;
        };
        return result;
    };
    return impl;
}

ToolImpl create_counting_write_tool(
    const std::shared_ptr<std::atomic<int>>& executions) {
    ToolImpl impl;
    impl.definition.name = "counting_write";
    impl.definition.description = "Test-only write tool.";
    impl.definition.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object()}
    };
    impl.execute = [executions](
                       const std::string&, const acecode::ToolContext&) {
        ++*executions;
        return ToolResult{"unexpected", true};
    };
    return impl;
}

// Fixture:封装 AgentLoop + stub + 消息收集器 + 完成同步。
class AgentLoopHarness {
public:
    explicit AgentLoopHarness(std::string cwd = ".",
        std::function<void(const acecode::TokenUsage&)> on_usage = {},
        std::shared_ptr<acecode::MemoryService> memory = nullptr, bool with_session = false)
        : cwd_(std::move(cwd)), memory_(memory) {
        tools_.register_tool(create_noop_tool());
        tools_.register_tool(acecode::create_task_complete_tool());

        AgentCallbacks cb;
        cb.on_usage = std::move(on_usage);
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

        if (with_session) session_manager_.start_session(cwd_, "stub", "stub-model");
        auto services = acecode_test::AgentLoopFixture::dependencies(
            provider_accessor, tools_, cb, perms_, with_session ? &session_manager_ : nullptr, nullptr, memory);
        services.prompt_config = [state = prompt_state_] {
            std::lock_guard<std::mutex> lock(state->mutex);
            ++state->captures;
            return state->config;
        };
        loop_ = std::make_unique<AgentLoop>(std::move(services),
            acecode_test::AgentLoopFixture::configuration(/*cwd=*/cwd_));
        loop_->start();
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



    std::vector<ChatMessage> persisted_session_messages() const {
        return session_manager_.load_active_messages();
    }

    std::vector<acecode::SessionTrajectoryRecord> persisted_trajectory() const {
        return acecode::SessionTrajectoryStorage::load_all(
            session_manager_.current_trajectory_path());
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
    void push_tool_calls(std::vector<acecode::ToolCall> calls) {
        ScriptedResponse response;
        response.tool_calls = std::move(calls);
        provider_->push_response(std::move(response));
    }
    // 同一条回复里既有正文又有原生工具调用。
    void push_text_with_tool_calls(std::string text,
                                   std::vector<acecode::ToolCall> calls) {
        ScriptedResponse response;
        response.text = std::move(text);
        response.tool_calls = std::move(calls);
        provider_->push_response(std::move(response));
    }
    void register_tool(ToolImpl tool) {
        tools_.register_tool(std::move(tool));
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

    void set_memory_config(const acecode::MemoryConfig* config) {
        std::lock_guard<std::mutex> lock(prompt_state_->mutex);
        prompt_state_->config.memory = config ? std::make_optional(*config) : std::nullopt;
        // 有记忆服务时开关以服务的运行时配置为准(与生产一致:设置保存即更新服务)。
        if (memory_ && config) memory_->update_config(*config);
    }

    void set_project_instructions_config(const acecode::ProjectInstructionsConfig* cfg) {
        std::lock_guard<std::mutex> lock(prompt_state_->mutex);
        prompt_state_->config.project_instructions = cfg ? std::make_optional(*cfg) : std::nullopt;
    }

    std::function<void()> custom_instruction_update(std::string text) {
        return [state = prompt_state_, text = std::move(text)] {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->config.custom_instructions.emplace();
            state->config.custom_instructions->set_text(text);
        };
    }

    int prompt_captures() const {
        std::lock_guard<std::mutex> lock(prompt_state_->mutex);
        return prompt_state_->captures;
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

    bool wait_for_event(acecode::SessionEventKind kind,
                        std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        std::unique_lock<std::mutex> lk(event_mu_);
        return event_cv_.wait_for(lk, timeout, [this, kind] {
            return std::any_of(events_.begin(), events_.end(), [kind](const auto& event) {
                return event.kind == kind;
            });
        });
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
    std::string cwd_;
    std::shared_ptr<acecode::MemoryService> memory_;
    std::shared_ptr<StubLlmProvider> provider_ = std::make_shared<StubLlmProvider>();
    ToolExecutor tools_;
    PermissionManager perms_;
    acecode::SessionManager session_manager_;
    struct PromptState {
        std::mutex mutex;
        acecode::SessionPromptConfig config;
        int captures = 0;
    };
    // 测试线程与工具线程共同持有发布状态，回合只拿值快照。
    std::shared_ptr<PromptState> prompt_state_ = std::make_shared<PromptState>();
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

std::vector<acecode::TurnNetDiffRecord> turn_net_diffs_from(
    const std::vector<ChatMessage>& messages) {
    std::vector<acecode::TurnNetDiffRecord> out;
    for (const auto& msg : messages) {
        if (!msg.metadata.is_object()) continue;
        auto record = acecode::decode_turn_net_diff(
            msg.metadata.value("turn_net_diff", nlohmann::json{}));
        if (record.has_value()) out.push_back(std::move(*record));
    }
    return out;
}

void expect_turn_diff_before_terminal(AgentLoopHarness& harness,
                                      const std::string& outcome) {
    ASSERT_TRUE(harness.wait_for_event(acecode::SessionEventKind::Done));
    const auto events = harness.snapshot_events();
    const auto turn_diff_it = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.kind == acecode::SessionEventKind::TurnDiff;
    });
    const auto terminal_busy_it = std::find_if(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == acecode::SessionEventKind::BusyChanged &&
               event.payload.is_object() &&
               !event.payload.value("busy", true) &&
               event.payload.value("outcome", std::string{}) == outcome;
    });
    const auto done_it = std::find_if(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == acecode::SessionEventKind::Done &&
               event.payload.value("outcome", std::string{}) == outcome;
    });
    ASSERT_NE(turn_diff_it, events.end());
    ASSERT_NE(terminal_busy_it, events.end());
    ASSERT_NE(done_it, events.end());
    EXPECT_LT(turn_diff_it, terminal_busy_it);
    EXPECT_LT(turn_diff_it, done_it);

    auto live_record = acecode::decode_turn_net_diff(turn_diff_it->payload);
    ASSERT_TRUE(live_record.has_value());
    EXPECT_TRUE(live_record->complete);
    EXPECT_TRUE(live_record->files.empty());

    const auto persisted = turn_net_diffs_from(harness.persisted_session_messages());
    ASSERT_EQ(persisted.size(), 1u);
    EXPECT_EQ(persisted[0].user_message_uuid, live_record->user_message_uuid);
    EXPECT_TRUE(persisted[0].complete);
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

TEST(AgentLoopTurnNetDiff, CompletedTurnPersistsAndEmitsBeforeTerminalEvents) {
    TempHomeGuard home("acecode_turn_net_diff_completed");
    const auto cwd = home.root() / "work";
    fs::create_directories(cwd);
    AgentLoopHarness h(cwd.string(), {}, nullptr, true);

    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("complete"));
    expect_turn_diff_before_terminal(h, "completed");
}

TEST(AgentLoopTurnNetDiff, ErrorTurnPersistsAndEmitsBeforeTerminalEvents) {
    TempHomeGuard home("acecode_turn_net_diff_error");
    const auto cwd = home.root() / "work";
    fs::create_directories(cwd);
    AgentLoopHarness h(cwd.string(), {}, nullptr, true);

    h.push_provider_error(make_stub_provider_error("provider failed"));

    ASSERT_TRUE(h.submit_and_wait("fail"));
    expect_turn_diff_before_terminal(h, "error");
}

TEST(AgentLoopTurnNetDiff, AbortedTurnPersistsAndEmitsBeforeTerminalEvents) {
    TempHomeGuard home("acecode_turn_net_diff_aborted");
    const auto cwd = home.root() / "work";
    fs::create_directories(cwd);
    AgentLoopHarness h(cwd.string(), {}, nullptr, true);

    h.set_stub_latency_ms(200);
    h.push_tool_call("noop", "{}", "c1");

    std::thread aborter([&h] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        h.abort();
    });
    ASSERT_TRUE(h.submit_and_wait("abort", std::chrono::seconds(10)));
    aborter.join();
    expect_turn_diff_before_terminal(h, "aborted");
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

TEST(AgentLoopTermination, RecoveryPreservesAccountedUsageWhenConsumerThrows) {
    for (bool reported : {true, false}) {
        SCOPED_TRACE(reported);
        acecode::TokenUsage accounted;
        AgentLoopHarness h(".", [&](const acecode::TokenUsage& usage) {
            accounted = usage;
            throw std::runtime_error("usage consumer failed");
        });
        acecode::StreamEvent delta;
        delta.type = acecode::StreamEventType::Delta;
        delta.content = "completed provider response";
        acecode::StreamEvent usage;
        usage.type = acecode::StreamEventType::Usage;
        usage.usage.prompt_tokens = 100;
        usage.usage.completion_tokens = 20;
        usage.usage.total_tokens = 120;
        usage.usage.has_data = reported;
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        h.push_events({delta, usage, done});
        ASSERT_TRUE(h.submit_and_wait("account before notifying consumers"));
        ASSERT_TRUE(h.wait_for_event(acecode::SessionEventKind::Done));
        ASSERT_GT(accounted.total_tokens, 0);
        int terminal_events = 0;
        for (const auto& event : h.snapshot_events()) {
            if (event.kind != acecode::SessionEventKind::Done &&
                !(event.kind == acecode::SessionEventKind::BusyChanged &&
                  !event.payload.value("busy", true))) continue;
            ++terminal_events;
            EXPECT_EQ(event.payload.value("outcome", ""), "error");
            const auto& total = event.payload.at("usage");
            EXPECT_EQ(total.at("prompt_tokens"), accounted.prompt_tokens);
            EXPECT_EQ(total.at("completion_tokens"), accounted.completion_tokens);
            EXPECT_EQ(total.at("total_tokens"), accounted.total_tokens);
            EXPECT_EQ(total.at("has_data"), reported);
        }
        EXPECT_EQ(terminal_events, 2);
    }
}

TEST(AgentLoopTermination, TerminalEventsExposeAggregateTurnUsage) {
    AgentLoopHarness h;

    acecode::StreamEvent tool_call;
    tool_call.type = acecode::StreamEventType::ToolCall;
    tool_call.tool_call = {"usage-call", "noop", "{}"};
    acecode::StreamEvent first_usage;
    first_usage.type = acecode::StreamEventType::Usage;
    first_usage.usage.prompt_tokens = 100;
    first_usage.usage.completion_tokens = 20;
    first_usage.usage.total_tokens = 120;
    first_usage.usage.cache_read_tokens = 60;
    first_usage.usage.reasoning_tokens = 5;
    first_usage.usage.has_data = true;
    acecode::StreamEvent first_done;
    first_done.type = acecode::StreamEventType::Done;
    first_done.finish_reason = "tool_calls";
    h.push_events({tool_call, first_usage, first_done});

    acecode::StreamEvent final_delta;
    final_delta.type = acecode::StreamEventType::Delta;
    final_delta.content = "finished";
    acecode::StreamEvent second_usage;
    second_usage.type = acecode::StreamEventType::Usage;
    second_usage.usage.prompt_tokens = 150;
    second_usage.usage.completion_tokens = 30;
    second_usage.usage.total_tokens = 180;
    second_usage.usage.cache_read_tokens = 90;
    second_usage.usage.cache_write_tokens = 4;
    second_usage.usage.reasoning_tokens = 7;
    second_usage.usage.has_data = true;
    acecode::StreamEvent second_done;
    second_done.type = acecode::StreamEventType::Done;
    second_done.finish_reason = "stop";
    h.push_events({final_delta, second_usage, second_done});

    ASSERT_TRUE(h.submit_and_wait("run two model steps"));
    ASSERT_TRUE(h.wait_for_event(acecode::SessionEventKind::Done));

    int step_usage_events = 0;
    nlohmann::json terminal_busy;
    nlohmann::json terminal_done;
    for (const auto& event : h.snapshot_events()) {
        if (event.kind == acecode::SessionEventKind::Usage) {
            ++step_usage_events;
        } else if (event.kind == acecode::SessionEventKind::BusyChanged &&
                   event.payload.is_object() &&
                   !event.payload.value("busy", true)) {
            terminal_busy = event.payload;
        } else if (event.kind == acecode::SessionEventKind::Done) {
            terminal_done = event.payload;
        }
    }

    EXPECT_EQ(step_usage_events, 2);
    ASSERT_TRUE(terminal_busy.is_object());
    ASSERT_TRUE(terminal_done.is_object());
    ASSERT_TRUE(terminal_busy.contains("usage"));
    ASSERT_TRUE(terminal_done.contains("usage"));
    EXPECT_FALSE(terminal_busy.value("turn_id", std::string{}).empty());
    EXPECT_EQ(terminal_busy["turn_id"], terminal_done["turn_id"]);
    EXPECT_EQ(terminal_busy["usage"], terminal_done["usage"]);

    const auto& usage = terminal_done["usage"];
    EXPECT_EQ(usage.value("prompt_tokens", 0), 250);
    EXPECT_EQ(usage.value("completion_tokens", 0), 50);
    EXPECT_EQ(usage.value("total_tokens", 0), 300);
    EXPECT_EQ(usage.value("cache_read_tokens", 0), 150);
    EXPECT_EQ(usage.value("cache_write_tokens", 0), 4);
    EXPECT_EQ(usage.value("reasoning_tokens", 0), 12);
    EXPECT_TRUE(usage.value("has_data", false));
}

TEST(AgentLoopTermination, EstimatedStepMarksAggregateTurnUsageAsEstimated) {
    AgentLoopHarness h;

    acecode::StreamEvent tool_call;
    tool_call.type = acecode::StreamEventType::ToolCall;
    tool_call.tool_call = {"estimated-usage-call", "noop", "{}"};
    acecode::StreamEvent reported_usage;
    reported_usage.type = acecode::StreamEventType::Usage;
    reported_usage.usage.prompt_tokens = 10;
    reported_usage.usage.completion_tokens = 5;
    reported_usage.usage.total_tokens = 15;
    reported_usage.usage.has_data = true;
    acecode::StreamEvent tool_done;
    tool_done.type = acecode::StreamEventType::Done;
    tool_done.finish_reason = "tool_calls";
    h.push_events({tool_call, reported_usage, tool_done});
    h.push_text("finished without provider usage");

    ASSERT_TRUE(h.submit_and_wait("run estimated model step"));
    ASSERT_TRUE(h.wait_for_event(acecode::SessionEventKind::Done));

    nlohmann::json terminal_done;
    for (const auto& event : h.snapshot_events()) {
        if (event.kind == acecode::SessionEventKind::Done) {
            terminal_done = event.payload;
        }
    }
    ASSERT_TRUE(terminal_done.contains("usage"));
    const auto& usage = terminal_done["usage"];
    EXPECT_GT(usage.value("prompt_tokens", 0), 10);
    EXPECT_GT(usage.value("completion_tokens", 0), 5);
    EXPECT_GT(usage.value("total_tokens", 0), 15);
    EXPECT_FALSE(usage.value("has_data", true));
    ASSERT_TRUE(usage.contains("context_breakdown"));
    const auto& context = usage["context_breakdown"];
    const int categorized_prompt =
        context.value("system_prompt", 0) +
        context.value("project_rules", 0) +
        context.value("skills", 0) +
        context.value("builtin_tools", 0) +
        context.value("mcp_tools", 0) +
        context.value("conversation", 0) +
        context.value("dynamic_context", 0);
    EXPECT_EQ(categorized_prompt, usage.value("prompt_tokens", 0));
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
// 秒级时间戳,于是每一轮工具往返都换一份内容,缓存前缀在此断开。cwd
// 留在静态 system prompt 里,动态日期不再进入该前缀,可变块只随内容变化。
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

    // 静态 system prompt 保留 cwd,但不得携带会跨日期改变的动态日期。
    EXPECT_EQ(first.front().role, "system");
    EXPECT_EQ(first.front().content.find("- Today's date: "), std::string::npos);
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

    auto memory = std::make_shared<acecode::MemoryService>(
        acecode::get_memory_dir(), acecode::get_memory_state_db_path(), acecode::MemoryConfig{});
    std::string err;
    ASSERT_TRUE(memory->global().upsert("user_profile", acecode::MemoryType::User,
                                        "senior Go dev", "10y Go\n",
                                        acecode::MemoryWriteMode::Create, err).has_value())
        << err;

    acecode::MemoryConfig memory_cfg;
    acecode::ProjectInstructionsConfig project_cfg;

    AgentLoopHarness h(repo.string(), {}, memory);
    h.set_memory_config(&memory_cfg);
    h.set_project_instructions_config(&project_cfg);
    h.push_text("ok");

    ASSERT_TRUE(h.submit_and_wait("what should I do?"));
    ASSERT_EQ(h.turn_count(), 1);

    auto request = h.request_messages_for_turn(0);
    ASSERT_GE(request.size(), 3u);
    ASSERT_EQ(request.front().role, "system");
    EXPECT_EQ(request.front().content.find("# Project Instructions"), std::string::npos);
    EXPECT_EQ(request.front().content.find("## Global memory"), std::string::npos);
    EXPECT_EQ(request.front().content.find("use goroutines"), std::string::npos);
    EXPECT_EQ(request.front().content.find("user_profile"), std::string::npos);

    bool saw_project = false;
    bool saw_memory = false;
    for (const auto& msg : request) {
        if (msg.content.find("# Project Instructions") != std::string::npos &&
            msg.content.find("use goroutines") != std::string::npos) {
            saw_project = true;
        }
        if (msg.content.find("## Global memory") != std::string::npos &&
            msg.content.find("user_profile") != std::string::npos) {
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
        EXPECT_EQ(msg.content.find("## Global memory"), std::string::npos);
        EXPECT_EQ(msg.content.find("<system-reminder>"), std::string::npos);
    }
}

// 场景:项目文件与记忆在会话中途都变了。
// 期望:项目指令按内容刷新;记忆上下文是会话快照,中途写入的条目不出现、已注入的
// 逐字节不变(openspec unify-memory-system:会话内快照稳定,不打穿 prompt cache);
// 静态 system prompt 字节不变。
TEST(AgentLoopTermination, MutableContextChangesDoNotChangeStaticSystemPrompt) {
    TempHomeGuard home("acecode-agentloop-context-edit");
    fs::path repo = home.root() / "repo";
    write_file(repo / "AGENT.md", "before rule\n");

    auto memory = std::make_shared<acecode::MemoryService>(
        acecode::get_memory_dir(), acecode::get_memory_state_db_path(), acecode::MemoryConfig{});
    std::string err;
    ASSERT_TRUE(memory->global().upsert("first_memory", acecode::MemoryType::User,
                                        "first memory", "before\n",
                                        acecode::MemoryWriteMode::Create, err).has_value())
        << err;

    acecode::MemoryConfig memory_cfg;
    acecode::ProjectInstructionsConfig project_cfg;

    AgentLoopHarness h(repo.string(), {}, memory);
    h.set_memory_config(&memory_cfg);
    h.set_project_instructions_config(&project_cfg);
    h.push_text("first ok");
    ASSERT_TRUE(h.submit_and_wait("first"));

    write_file(repo / "AGENT.md", "after rule\n");
    ASSERT_TRUE(memory->global().upsert("second_memory", acecode::MemoryType::User,
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
    EXPECT_TRUE(contains(first_request, "first_memory"));
    EXPECT_FALSE(contains(first_request, "second_memory"));

    EXPECT_TRUE(contains(second_request, "after rule"));
    EXPECT_TRUE(contains(second_request, "first_memory"));
    EXPECT_FALSE(contains(second_request, "second_memory"));
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

    const auto messages = h.persisted_messages();
    const auto tool_result = std::find_if(
        messages.rbegin(), messages.rend(), [](const ChatMessage& message) {
            return message.role == "tool" && message.tool_call_id == "c-done";
        });
    ASSERT_NE(tool_result, messages.rend());

    const auto events = h.snapshot_events();
    const auto tool_end = std::find_if(
        events.begin(), events.end(), [](const acecode::SessionEvent& event) {
            return event.kind == acecode::SessionEventKind::ToolEnd &&
                   event.payload.value("tool", std::string{}) == "task_complete";
        });
    ASSERT_NE(tool_end, events.end());
    ASSERT_TRUE(tool_end->payload.contains("message_id"));
    EXPECT_EQ(tool_end->payload["message_id"],
              acecode::web::compute_message_id(*tool_result));
}

TEST(AgentLoopTermination, TerminalSessionActionRunsAfterDoneAndStopsLaterWrites) {
    AgentLoopHarness h(".", {}, nullptr, true);

    auto terminal_executions = std::make_shared<std::atomic<int>>(0);
    auto later_write_executions = std::make_shared<std::atomic<int>>(0);
    auto post_turn_actions = std::make_shared<std::atomic<int>>(0);
    h.register_tool(create_terminal_session_tool(
        terminal_executions, post_turn_actions));
    h.register_tool(create_counting_write_tool(later_write_executions));
    h.push_tool_calls({
        {"terminal-call", "terminal_session_action", "{}"},
        {"later-call", "counting_write", "{}"},
    });

    ASSERT_TRUE(h.submit_and_wait("delete this session"));
    ASSERT_TRUE(h.wait_for_event(acecode::SessionEventKind::Done));
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (post_turn_actions->load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_EQ(terminal_executions->load(), 1);
    EXPECT_EQ(later_write_executions->load(), 0);
    EXPECT_EQ(post_turn_actions->load(), 1);
    EXPECT_EQ(h.turn_count(), 1);

    const auto persisted = h.persisted_session_messages();
    EXPECT_TRUE(std::any_of(persisted.begin(), persisted.end(), [](const auto& msg) {
        return msg.role == "tool" && msg.content == "[Interrupted]";
    }));
    EXPECT_FALSE(turn_timings_from(persisted).empty());
}

TEST(AgentLoopTermination, TaskCompleteLiveMessageIdMatchesBudgetedCanonicalResult) {
    TempHomeGuard temp_home("acecode-task-complete-message-id");
    AgentLoopHarness h(temp_home.root().string(), {}, nullptr, true);

    h.push_task_complete(std::string(
        acecode::TOOL_RESULT_DEFAULT_MAX_BYTES + 1024, 'x'));

    ASSERT_TRUE(h.submit_and_wait("finish with a large summary"));
    const auto messages = h.persisted_session_messages();
    const auto tool_result = std::find_if(
        messages.rbegin(), messages.rend(), [](const ChatMessage& message) {
            return message.role == "tool" && message.tool_call_id == "c-done";
        });
    ASSERT_NE(tool_result, messages.rend());
    EXPECT_TRUE(acecode::is_persisted_output_message(tool_result->content));

    const auto events = h.snapshot_events();
    const auto tool_end = std::find_if(
        events.begin(), events.end(), [](const acecode::SessionEvent& event) {
            return event.kind == acecode::SessionEventKind::ToolEnd &&
                   event.payload.value("tool", std::string{}) == "task_complete";
        });
    ASSERT_NE(tool_end, events.end());
    EXPECT_EQ(tool_end->payload["message_id"],
              acecode::web::compute_message_id(*tool_result));

    const auto trajectory = h.persisted_trajectory();
    const auto persisted_tool_end = std::find_if(
        trajectory.begin(), trajectory.end(), [](const auto& record) {
            return record.type == "tool_end" &&
                   record.payload.value("tool", std::string{}) == "task_complete";
        });
    ASSERT_NE(persisted_tool_end, trajectory.end());
    EXPECT_EQ(persisted_tool_end->payload["message_id"],
              acecode::web::compute_message_id(*tool_result));
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

namespace {

std::string first_tool_result_content(const std::vector<ChatMessage>& messages) {
    for (const auto& m : messages) {
        if (m.role == "tool") return m.content;
    }
    return {};
}

} // namespace

// 场景:模型调了一个不存在的工具 `nope`(yubo2 现场是 `exec`)。
// 期望:工具结果除了 Unknown tool 外,还列出**本次请求**发给模型的模型侧工具名
// (noop / task_complete),让模型照抄;「工具重写」生效时列表里是模型侧名
// do_nothing,不出现它不认识的原生名 noop。
// 回归:旧文案只有 "Unknown tool: nope",模型只能继续瞎猜工具名。
TEST(AgentLoopTermination, UnknownToolErrorListsModelFacingNames) {
    {
        acecode::ScopedModelToolNameMappings none(acecode::ToolProtocolNameMappings{});
        AgentLoopHarness h;
        h.push_tool_call("nope", "{}", "c-unknown");
        h.push_text("done");
        ASSERT_TRUE(h.submit_and_wait("do it"));

        const std::string result = first_tool_result_content(h.persisted_messages());
        EXPECT_NE(result.find("Unknown tool: nope"), std::string::npos) << result;
        EXPECT_NE(result.find("Available tools:"), std::string::npos) << result;
        EXPECT_NE(result.find("noop"), std::string::npos) << result;
        EXPECT_NE(result.find("task_complete"), std::string::npos) << result;
    }
    {
        acecode::ScopedModelToolNameMappings mapped{{"noop", "do_nothing"}};
        AgentLoopHarness h;
        h.push_tool_call("nope", "{}", "c-unknown");
        h.push_text("done");
        ASSERT_TRUE(h.submit_and_wait("do it"));

        const std::string result = first_tool_result_content(h.persisted_messages());
        EXPECT_NE(result.find("Available tools:"), std::string::npos) << result;
        EXPECT_NE(result.find("do_nothing"), std::string::npos) << result;
        EXPECT_EQ(result.find("noop"), std::string::npos) << result;
    }
}

// 场景:模型把工具名写成 `NOOP`(只大小写不同,且只有一个候选)。
// 期望:大小写容错解析到原生 noop 并成功执行,结果里没有 Unknown tool。
// 回归:旧实现原样透传 `NOOP`,工具不执行、报 Unknown tool。
TEST(AgentLoopTermination, MixedCaseToolCallExecutesRegisteredTool) {
    acecode::ScopedModelToolNameMappings none(acecode::ToolProtocolNameMappings{});
    AgentLoopHarness h;
    h.push_tool_call("NOOP", "{}", "c-mixed");
    h.push_text("done");
    ASSERT_TRUE(h.submit_and_wait("do it"));

    const auto messages = h.persisted_messages();
    const std::string result = first_tool_result_content(messages);
    EXPECT_EQ(result.find("Unknown tool"), std::string::npos) << result;
    EXPECT_NE(result.find("ok"), std::string::npos) << result;
    bool saw_native_call = false;
    for (const auto& m : messages) {
        if (m.role != "assistant" || !m.tool_calls.is_array()) continue;
        for (const auto& tc : m.tool_calls) {
            if (tc.value("function", nlohmann::json::object())
                    .value("name", std::string()) == "noop") {
                saw_native_call = true;
            }
        }
    }
    EXPECT_TRUE(saw_native_call);
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

// ---- 文本形式工具调用的纠正重试(fix-feedback-0924 第 3 条)----
//
// provider(OpenAiCompatProvider)认出模型把调用写进了正文、却无法执行时,
// 在 Done 事件上报 Outcome::Rejected。这里用 StubLlmProvider 直接脚本化
// 「provider 已经藏起标记后的可见正文 + Done 上的诊断」,只测 AgentLoop 侧。

namespace {

acecode::TextToolCallDiagnostic make_rejected_text_tool_call(
    std::string format = "invoke",
    std::string reason = "unknown_tool",
    std::string error = "tool \"exec\" is not available",
    std::vector<std::string> tools = {"exec"}) {
    acecode::TextToolCallDiagnostic diag;
    diag.outcome = acecode::TextToolCallDiagnostic::Outcome::Rejected;
    diag.format = std::move(format);
    diag.reason = std::move(reason);
    diag.error = std::move(error);
    diag.attempted_tools = std::move(tools);
    diag.raw_excerpt =
        "<invoke name=\"exec\">\n<parameter name=\"command\">\nls\n</parameter>\n</invoke>";
    return diag;
}

std::vector<acecode::StreamEvent> make_text_tool_call_response(
    const std::string& visible,
    acecode::TextToolCallDiagnostic diag,
    const std::string& finish_reason = "stop") {
    std::vector<acecode::StreamEvent> events;
    if (!visible.empty()) {
        acecode::StreamEvent delta;
        delta.type = acecode::StreamEventType::Delta;
        delta.content = visible;
        events.push_back(std::move(delta));
    }
    acecode::StreamEvent done;
    done.type = acecode::StreamEventType::Done;
    done.finish_reason = finish_reason;
    done.text_tool_calls = std::move(diag);
    events.push_back(std::move(done));
    return events;
}

int count_user_messages_flagged(const std::vector<ChatMessage>& messages,
                                const std::string& flag) {
    int n = 0;
    for (const auto& msg : messages) {
        if (msg.role == "user" && msg.metadata.is_object() &&
            msg.metadata.value(flag, false)) {
            EXPECT_TRUE(msg.metadata.value("hidden_goal_context", false));
            ++n;
        }
    }
    return n;
}

std::vector<nlohmann::json> notice_params_for(
    const std::vector<acecode::SessionEvent>& events, const std::string& code) {
    std::vector<nlohmann::json> out;
    for (const auto& event : events) {
        if (event.kind != acecode::SessionEventKind::Message ||
            !event.payload.is_object()) {
            continue;
        }
        const auto metadata = event.payload.value("metadata", nlohmann::json::object());
        if (!metadata.is_object() || !metadata.contains("system_notice")) continue;
        const auto& notice = metadata["system_notice"];
        if (notice.value("code", std::string{}) == code) {
            out.push_back(notice.value("params", nlohmann::json::object()));
        }
    }
    return out;
}

const ChatMessage* find_rejected_assistant(const std::vector<ChatMessage>& messages) {
    for (const auto& msg : messages) {
        if (msg.role == "assistant" && msg.metadata.is_object() &&
            msg.metadata.contains("text_tool_call_rejected")) {
            return &msg;
        }
    }
    return nullptr;
}

} // namespace

// 场景:yubo2 现场形态 —— 模型把 `<invoke name="exec">` 写进正文,provider 认出
// 意图但工具不存在(Rejected/unknown_tool),藏起标记后可见正文只剩 "\n\n\n"。
// 期望:注入隐藏纠正提示(列出本次请求的模型侧工具名)并重试,第 2 轮正常回复;
// 发 response_text_tool_call_retry 通知;被拒 assistant 落盘为空串且带诊断
// metadata;任何落盘 / 发给模型的正文都不含 `<invoke`。
// 回归:旧实现第 1 步就走 `Text-only response; ending loop`,回合静默结束,
// 模型以为调用执行了,用户什么结果也拿不到。
TEST(AgentLoopTermination, RejectedTextToolCallInjectsCorrectionAndRecovers) {
    AgentLoopHarness h;
    h.push_events(make_text_tool_call_response("\n\n\n", make_rejected_text_tool_call()));
    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("list files"));
    EXPECT_EQ(h.turn_count(), 2);
    EXPECT_EQ(h.count_by_role("error"), 0);
    // 被拒回复内容为空,不 dispatch(不出空气泡);只有第 2 轮的回复。
    EXPECT_EQ(h.count_by_role("assistant"), 1);

    const auto persisted = h.persisted_messages();
    const ChatMessage* rejected = find_rejected_assistant(persisted);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->content, "");
    const auto& diag_json = rejected->metadata["text_tool_call_rejected"];
    EXPECT_EQ(diag_json.value("format", std::string{}), "invoke");
    EXPECT_EQ(diag_json.value("reason", std::string{}), "unknown_tool");
    EXPECT_EQ(count_user_messages_flagged(persisted, "text_tool_call_correction"), 1);
    for (const auto& msg : persisted) {
        EXPECT_EQ(msg.content.find("<invoke"), std::string::npos) << msg.content;
    }

    const auto second_request = h.request_messages_for_turn(1);
    bool prompt_in_request = false;
    for (const auto& msg : second_request) {
        EXPECT_EQ(msg.content.find("<invoke"), std::string::npos) << msg.content;
        if (msg.role == "user" &&
            msg.content.find("native tool-calling") != std::string::npos) {
            prompt_in_request = true;
            EXPECT_NE(msg.content.find("Available tools:"), std::string::npos);
            EXPECT_NE(msg.content.find("noop"), std::string::npos);
            EXPECT_NE(msg.content.find("tool \"exec\" is not available"),
                      std::string::npos);
        }
    }
    EXPECT_TRUE(prompt_in_request);

    const auto notices = notice_params_for(h.snapshot_events(),
                                           "response_text_tool_call_retry");
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].value("attempt", 0), 1);
    EXPECT_EQ(notices[0].value("attempts", 0), 2);
    EXPECT_EQ(notices[0].value("error", std::string{}), "tool \"exec\" is not available");
}

// 场景:被拒回复藏起标记后只剩空白,与「成功但空」的回复在内容上无法区分。
// 期望:走文本调用纠正(Rejected 处理在 response_is_blank 之前),不发
// [空回复] 通知、不注入空回复提示。
// 回归:若把 Rejected 处理放在空回复判断之后,模型会收到「你的回复是空的」
// 这种错误的纠正文案,永远不知道问题是调用格式。
TEST(AgentLoopTermination, RejectedTextToolCallIsNotMistakenForEmptyResponse) {
    AgentLoopHarness h;
    h.push_events(make_text_tool_call_response("\n\n\n\n", make_rejected_text_tool_call()));
    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(h.turn_count(), 2);
    const auto persisted = h.persisted_messages();
    EXPECT_EQ(count_user_messages_flagged(persisted, "empty_response_retry"), 0);
    EXPECT_EQ(count_user_messages_flagged(persisted, "text_tool_call_correction"), 1);
    EXPECT_TRUE(notice_params_for(h.snapshot_events(), "response_empty_retry").empty());
    for (const auto& m : h.snapshot_messages()) {
        EXPECT_EQ(m.content.find(u8"[空回复]"), std::string::npos) << m.content;
    }
}

// 场景:模型连续 3 次(首轮 + 2 次纠正)都把调用写成无法执行的正文。
// 期望:纠正上限 2 次耗尽后发可见 error(说明原因与最后一次错误),回合以
// error 结束(turn timing = error),不再无限重试烧 token。
TEST(AgentLoopTermination, RejectedTextToolCallExhaustsCorrectionsEndsWithError) {
    AgentLoopHarness h;
    for (int i = 0; i < 3; ++i) {
        h.push_events(make_text_tool_call_response("\n\n\n", make_rejected_text_tool_call()));
    }
    h.push_text("never reached");

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(h.turn_count(), 3);
    EXPECT_EQ(h.count_by_role("error"), 1);
    bool saw_error = false;
    for (const auto& m : h.snapshot_messages()) {
        if (m.role == "error" &&
            m.content.find(u8"连续 3 次把工具调用写成正文文本") != std::string::npos &&
            m.content.find("tool \"exec\" is not available") != std::string::npos) {
            saw_error = true;
        }
    }
    EXPECT_TRUE(saw_error);

    const auto persisted = h.persisted_messages();
    EXPECT_EQ(count_user_messages_flagged(persisted, "text_tool_call_correction"), 2);
    EXPECT_EQ(notice_params_for(h.snapshot_events(), "response_text_tool_call_retry").size(), 2u);
    auto timings = turn_timings_from(persisted);
    ASSERT_EQ(timings.size(), 1u);
    EXPECT_EQ(timings[0].status, "error");
}

// 场景:被拒、被拒、原生调用、被拒、被拒、正文。
// 期望:纠正按「连续」次数计,中间那次原生调用把计数清零,所以 4 次被拒都能
// 纠正,回合正常完成、没有 error。
TEST(AgentLoopTermination, TextToolCallCorrectionCounterIsConsecutive) {
    AgentLoopHarness h;
    h.push_events(make_text_tool_call_response("", make_rejected_text_tool_call()));
    h.push_events(make_text_tool_call_response("", make_rejected_text_tool_call()));
    h.push_tool_call("noop", "{}", "c-native");
    h.push_events(make_text_tool_call_response("", make_rejected_text_tool_call()));
    h.push_events(make_text_tool_call_response("", make_rejected_text_tool_call()));
    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(h.turn_count(), 6);
    EXPECT_EQ(h.count_by_role("error"), 0);
    EXPECT_EQ(count_user_messages_flagged(h.persisted_messages(),
                                          "text_tool_call_correction"), 4);
}

// 场景:DSML 调用解析失败(网关把 DeepSeek 原生协议漏进了正文且格式坏了),
// 纠正 1 次后模型恢复。
// 期望:纠正上限只有 1 次(通知里 attempts=1),第 2 轮恢复后无 error。
// DSML 只纠正 1 次的理由:那不是模型「写错了格式」,纠正文本作用有限,多试
// 只是白烧整段上下文。
TEST(AgentLoopTermination, DsmlRejectedGetsSingleCorrectionThenRecovers) {
    AgentLoopHarness h;
    h.push_events(make_text_tool_call_response(
        "", make_rejected_text_tool_call("dsml", "parse_error", "unterminated DSML invoke", {})));
    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(h.turn_count(), 2);
    EXPECT_EQ(h.count_by_role("error"), 0);
    const auto notices = notice_params_for(h.snapshot_events(),
                                           "response_text_tool_call_retry");
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].value("attempts", 0), 1);
}

// 场景:DSML 连续 2 次解析失败。
// 期望:1 次纠正后即耗尽,回合以 error 结束(文案为「连续 2 次」)。
TEST(AgentLoopTermination, DsmlRejectedTwiceEndsWithError) {
    AgentLoopHarness h;
    for (int i = 0; i < 2; ++i) {
        h.push_events(make_text_tool_call_response(
            "", make_rejected_text_tool_call("dsml", "parse_error", "unterminated DSML invoke", {})));
    }
    h.push_text("never reached");

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(h.turn_count(), 2);
    EXPECT_EQ(h.count_by_role("error"), 1);
    bool saw_error = false;
    for (const auto& m : h.snapshot_messages()) {
        if (m.role == "error" &&
            m.content.find(u8"连续 2 次") != std::string::npos) {
            saw_error = true;
        }
    }
    EXPECT_TRUE(saw_error);
}

// 场景:文本调用写到一半被输出长度上限截断(provider 报 truncated,Done 的
// finish_reason=length)。
// 期望:AgentLoop 把原因细分成 truncated_by_length,纠正提示专门说明「撞上
// 输出长度上限、把大内容拆成多次小调用」,否则模型重发同样大的调用还会被截。
TEST(AgentLoopTermination, TruncatedByLengthCorrectionMentionsOutputLimit) {
    AgentLoopHarness h;
    h.push_events(make_text_tool_call_response(
        "",
        make_rejected_text_tool_call(
            "invoke", "truncated",
            "response ended inside a text tool call (missing </parameter>)", {"file_write"}),
        "length"));
    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("write a big file"));
    EXPECT_EQ(h.turn_count(), 2);
    const auto persisted = h.persisted_messages();
    const ChatMessage* rejected = find_rejected_assistant(persisted);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->metadata["text_tool_call_rejected"].value("reason", std::string{}),
              "truncated_by_length");

    bool mentions_limit = false;
    for (const auto& msg : h.request_messages_for_turn(1)) {
        if (msg.role == "user" &&
            msg.content.find("output length limit") != std::string::npos) {
            mentions_limit = true;
        }
    }
    EXPECT_TRUE(mentions_limit);
}

// 场景:可疑级 —— 调用标记写在正文行中(`Let me run <invoke name="bash">…`),
// 执行级认不出,标记已经流到界面;provider 报 Rejected/malformed,visible_cut
// 指向该行行首。
// 期望:落盘内容截到 visible_cut 并去掉末尾空白 = "好的,我来看看。";这条定稿
// 消息经 dispatch 替换流式草稿(界面上的标记随之消失);照常纠正重试。
TEST(AgentLoopTermination, SuspiciousTextToolCallTruncatesPersistedContent) {
    AgentLoopHarness h;
    const std::string prose = u8"好的,我来看看。\n";
    const std::string visible =
        prose + "Let me run <invoke name=\"bash\">\n<parameter name=\"command\">ls";
    auto diag = make_rejected_text_tool_call(
        "invoke", "malformed",
        "the reply contains tool-call markup that could not be parsed as a complete call",
        {});
    diag.visible_cut = prose.size();
    h.push_events(make_text_tool_call_response(visible, diag));
    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(h.turn_count(), 2);
    const auto persisted = h.persisted_messages();
    const ChatMessage* rejected = find_rejected_assistant(persisted);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->content, u8"好的,我来看看。");

    bool dispatched_truncated = false;
    for (const auto& m : h.snapshot_messages()) {
        if (m.role == "assistant") {
            EXPECT_EQ(m.content.find("<invoke"), std::string::npos) << m.content;
            if (m.content == u8"好的,我来看看。") dispatched_truncated = true;
        }
    }
    EXPECT_TRUE(dispatched_truncated);
    EXPECT_EQ(count_user_messages_flagged(h.persisted_messages(),
                                          "text_tool_call_correction"), 1);
}

// 场景:同一回复里既有原生调用 noop,又有与之不一致的文本调用 bash(yubo2 现场
// 第 1758 行形态),provider 报 IgnoredWithNative。
// 期望:只执行原生调用;批次跑完后追加一条隐藏说明,列出未执行的 bash(command),
// 并进入下一次请求;不消耗纠正预算、不发界面通知。
// 回归:旧实现只执行原生调用,Bash 没执行,模型却以为执行了。
TEST(AgentLoopTermination, IgnoredTextToolCallAppendsHiddenNoteAfterBatch) {
    AgentLoopHarness h;
    acecode::StreamEvent call;
    call.type = acecode::StreamEventType::ToolCall;
    call.tool_call.id = "c-native";
    call.tool_call.function_name = "noop";
    call.tool_call.function_arguments = "{}";
    call.tool_index = 0;
    acecode::StreamEvent done;
    done.type = acecode::StreamEventType::Done;
    done.finish_reason = "tool_calls";
    done.text_tool_calls.outcome =
        acecode::TextToolCallDiagnostic::Outcome::IgnoredWithNative;
    done.text_tool_calls.format = "invoke";
    done.text_tool_calls.attempted_tools = {"Bash"};
    done.text_tool_calls.unexecuted_detail = {"bash(command)"};
    h.push_events({call, done});
    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(h.turn_count(), 2);
    EXPECT_EQ(h.count_by_role("error"), 0);

    const auto persisted = h.persisted_messages();
    std::size_t tool_result_index = persisted.size();
    std::size_t note_index = persisted.size();
    for (std::size_t i = 0; i < persisted.size(); ++i) {
        if (persisted[i].role == "tool" && persisted[i].tool_call_id == "c-native") {
            tool_result_index = i;
        }
        if (persisted[i].role == "user" && persisted[i].metadata.is_object() &&
            persisted[i].metadata.value("text_tool_call_ignored", false)) {
            note_index = i;
            EXPECT_TRUE(persisted[i].metadata.value("hidden_goal_context", false));
            EXPECT_NE(persisted[i].content.find("bash(command)"), std::string::npos);
            EXPECT_NE(persisted[i].content.find("NOT executed"), std::string::npos);
        }
    }
    ASSERT_LT(tool_result_index, persisted.size());
    ASSERT_LT(note_index, persisted.size());
    EXPECT_LT(tool_result_index, note_index);

    bool note_in_request = false;
    for (const auto& msg : h.request_messages_for_turn(1)) {
        if (msg.role == "user" && msg.content.find("bash(command)") != std::string::npos) {
            note_in_request = true;
        }
    }
    EXPECT_TRUE(note_in_request);
    EXPECT_EQ(count_user_messages_flagged(persisted, "text_tool_call_correction"), 0);
    EXPECT_TRUE(notice_params_for(h.snapshot_events(),
                                  "response_text_tool_call_retry").empty());
}

// 场景:provider 扣住疑似文本调用期间发进度 delta(text_tool_call_hold=true,
// tool_index=-1,工具名为空),随后扣住的内容被释放成正文。
// 期望:进度 delta 不记 model_first_output;首次输出按真正的正文记为 content。
// 回归:若按普通 ToolCallDelta 处理,首次输出会被误记成 tool_call,trajectory
// 的首 token 统计与实际不符。
TEST(AgentLoopTermination, TextToolCallHoldDeltaDoesNotRecordFirstOutputChannel) {
    TempHomeGuard temp_home("acecode-text-tool-call-hold");
    AgentLoopHarness h(temp_home.root().string(), {}, nullptr, true);

    acecode::StreamEvent hold;
    hold.type = acecode::StreamEventType::ToolCallDelta;
    hold.tool_index = -1;
    hold.text_tool_call_hold = true;
    hold.tool_call_argument_bytes = 2048;
    acecode::StreamEvent delta;
    delta.type = acecode::StreamEventType::Delta;
    delta.content = "<invoke is just prose here";
    acecode::StreamEvent done;
    done.type = acecode::StreamEventType::Done;
    h.push_events({hold, delta, done});

    ASSERT_TRUE(h.submit_and_wait("go"));
    int first_outputs = 0;
    for (const auto& record : h.persisted_trajectory()) {
        if (record.type != "model_first_output") continue;
        ++first_outputs;
        EXPECT_EQ(record.payload.value("channel", std::string{}), "content");
    }
    EXPECT_EQ(first_outputs, 1);

    bool saw_hold_progress = false;
    for (const auto& event : h.snapshot_events()) {
        if (event.kind == acecode::SessionEventKind::AgentProgress &&
            event.payload.value("label", std::string{}) == u8"正在准备工具调用") {
            saw_hold_progress = true;
        }
    }
    EXPECT_TRUE(saw_hold_progress);
}

// 场景:max_iterations=2,前两轮都被拒、纠正后第 3 轮正常回复。
// 期望:纠正轮不计入 max_iterations(与空回复重试一致),3 次请求都发出,回合
// 正常完成,不触发 iteration_limit;计数回退有 >0 保护,不会下溢。
// 阈值取 2 而不是 1:现有收尾检查在 total_iterations >= max_iterations 时就报
// iteration_limit,max_iterations=1 时连一次普通文本回复都会报,测不出区别;
// 取 2 时若纠正轮被计数,第 2 次被拒后循环即停、只发 2 次请求。
TEST(AgentLoopTermination, CorrectionDoesNotUnderflowIterationCounter) {
    AgentLoopHarness h;
    acecode::AgentLoopConfig cfg;
    cfg.max_iterations = 2;
    h.set_config(cfg);
    h.push_events(make_text_tool_call_response("", make_rejected_text_tool_call()));
    h.push_events(make_text_tool_call_response("", make_rejected_text_tool_call()));
    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(h.turn_count(), 3);
    EXPECT_EQ(h.count_by_role("error"), 0);
    EXPECT_TRUE(notice_params_for(h.snapshot_events(), "iteration_limit").empty());
    EXPECT_EQ(h.last_system_message().find("max_iterations"), std::string::npos);
}

namespace {

// 计数型只读工具:断言「损坏那一步的工具调用没有被执行」。
ToolImpl create_counting_tool(const std::string& name,
                              std::shared_ptr<std::atomic<int>> runs) {
    ToolDef def;
    def.name = name;
    def.description = "Counts executions for agent-loop tests.";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object()}
    };
    ToolImpl impl;
    impl.definition = def;
    impl.execute = [runs](const std::string&, const acecode::ToolContext&) {
        runs->fetch_add(1);
        return ToolResult{"counted", true};
    };
    impl.is_read_only = true;
    impl.source = ToolSource::Builtin;
    return impl;
}

acecode::ToolCall probe_call(const std::string& id) {
    acecode::ToolCall call;
    call.id = id;
    call.function_name = "probe";
    call.function_arguments = "{}";
    return call;
}

// 反馈 huangyuan816 第一条回复的开头与结尾(一串数字 + 结尾的 </arg_value>)。
const char* const kCorruptedReply =
    " roots\n# 3.3# 4</think>5 4}\n443void\n38 id\n37\n41id\n40</arg_value>";

bool assistant_said(const std::vector<ChatMessage>& messages,
                    const std::string& needle) {
    for (const auto& msg : messages) {
        if (msg.role == "assistant" &&
            msg.content.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// 场景:模型第一条回复的正文混进了工具参数模板标记(</arg_value>),同一回复里
// 还带着一个原生工具调用;重发后正常回答。
// 期望:损坏那一步整条丢弃 —— 工具不执行、正文和调用都不入历史,两次请求的
// 消息逐条相同(原样重发);发一条 response_corrupted_retry 通知(1/1,带出
// 命中的标记);最终回复正常落盘,没有错误。
// 回归背景(反馈 huangyuan816,0.9.27):坏回复里的 bash 命令也夹着乱码
// (`ls …/2&&`),照样被执行,之后整个回合跑题、答非所问。
TEST(AgentLoopTermination, CorruptedOutputIsDiscardedAndRequestedAgainOnce) {
    AgentLoopHarness h;
    auto runs = std::make_shared<std::atomic<int>>(0);
    h.register_tool(create_counting_tool("probe", runs));
    h.push_text_with_tool_calls(kCorruptedReply, {probe_call("bad-1")});
    h.push_text("recovered answer");

    ASSERT_TRUE(h.submit_and_wait("why is the gif slow"));
    EXPECT_EQ(h.turn_count(), 2);
    EXPECT_EQ(runs->load(), 0) << "损坏那一步的工具调用不能执行";
    EXPECT_EQ(h.count_by_role("error"), 0);

    const auto first = h.request_messages_for_turn(0);
    const auto second = h.request_messages_for_turn(1);
    ASSERT_EQ(first.size(), second.size()) << "丢弃后原样重发,历史不能多出消息";
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].content, second[i].content) << "message " << i;
    }

    const auto persisted = h.persisted_messages();
    for (const auto& msg : persisted) {
        if (msg.role == "system") continue;  // 通知本身会复述命中的标记
        EXPECT_EQ(msg.content.find("</arg_value>"), std::string::npos)
            << msg.content;
        EXPECT_NE(msg.tool_call_id, "bad-1");
        if (msg.tool_calls.is_array()) {
            for (const auto& call : msg.tool_calls) {
                EXPECT_NE(call.value("id", std::string{}), "bad-1");
            }
        }
    }
    EXPECT_TRUE(assistant_said(persisted, "recovered answer"));

    const auto notices = notice_params_for(h.snapshot_events(),
                                           "response_corrupted_retry");
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].value("attempt", 0), 1);
    EXPECT_EQ(notices[0].value("attempts", 0), 1);
    EXPECT_EQ(notices[0].value("marker", std::string{}), "</arg_value>");
}

// 场景:重发之后的回复仍然带着模板标记。
// 期望:只重发一次,第二次按原流程当普通回复收下,回合正常结束,不报错。
// 连续两次损坏多半是服务端持续异常,再重发只会烧 token、把回合卡住。
TEST(AgentLoopTermination, CorruptedOutputIsRequestedAgainOnlyOnce) {
    AgentLoopHarness h;
    h.push_text(kCorruptedReply);
    h.push_text("still broken <arg_key>x</arg_key>");

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(h.turn_count(), 2);
    EXPECT_EQ(h.count_by_role("error"), 0);
    EXPECT_EQ(notice_params_for(h.snapshot_events(),
                                "response_corrupted_retry").size(), 1u);
    EXPECT_TRUE(assistant_said(h.persisted_messages(), "still broken"));
}

// 场景:损坏 → 重发得到一次正常的工具调用 → 下一步又损坏 → 再重发得到正常回答。
// 期望:重发名额按「连续」计,正常产出一次工具批次后清零,所以第二次损坏
// 同样会被丢弃重发一次:共 4 次请求、2 条通知、没有错误。
TEST(AgentLoopTermination, CorruptedOutputAllowanceResetsAfterValidToolStep) {
    AgentLoopHarness h;
    h.push_text(kCorruptedReply);
    h.push_tool_call("noop", "{}", "c1");
    h.push_text(kCorruptedReply);
    h.push_text("done");

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(h.turn_count(), 4);
    EXPECT_EQ(h.count_by_role("error"), 0);
    EXPECT_EQ(notice_params_for(h.snapshot_events(),
                                "response_corrupted_retry").size(), 2u);
    EXPECT_TRUE(assistant_said(h.persisted_messages(), "done"));
}

// 场景:修复上线前落盘的老会话里有一条 assistant 消息整条是文本工具调用(测试
// stub 不经过 provider 的文本调用恢复,原样返回,等价于旧版本落盘的消息)。
// 期望:下一回合发给模型的历史里它被换成固定说明,不再出现 <invoke;同一回合
// 相邻两次请求的公共前缀逐字节相同(清洗只由内容决定);落盘的历史保持原样。
// 回归表现:yubo2 的活跃会话在下一次压缩前,模型一直照着历史里的样本写文本调用。
TEST(AgentLoopTermination, LegacyTextToolCallHistoryIsScrubbedInRequest) {
    AgentLoopHarness h;
    h.push_text("\n\n<invoke name=\"Bash\">\n<parameter name=\"command\">\nls\n"
                "</parameter>\n</invoke>");
    ASSERT_TRUE(h.submit_and_wait("look around"));

    h.push_tool_call("noop", "{}", "c1");
    h.push_text("done");
    ASSERT_TRUE(h.submit_and_wait("continue"));
    ASSERT_EQ(h.turn_count(), 3);

    const auto second = h.request_messages_for_turn(1);
    const auto third = h.request_messages_for_turn(2);
    bool found_placeholder = false;
    for (const auto& m : second) {
        EXPECT_EQ(m.content.find("<invoke"), std::string::npos)
            << "legacy text tool call leaked into the request";
        if (m.role == "assistant" &&
            m.content == acecode::kTextToolCallHistoryPlaceholder) {
            found_placeholder = true;
        }
    }
    EXPECT_TRUE(found_placeholder);

    ASSERT_GT(third.size(), second.size());
    for (std::size_t i = 0; i < second.size(); ++i) {
        EXPECT_EQ(second[i].content, third[i].content)
            << "prompt prefix changed at message " << i;
    }

    bool raw_persisted = false;
    for (const auto& m : h.persisted_messages()) {
        if (m.role == "assistant" && m.content.find("<invoke") != std::string::npos) {
            raw_persisted = true;
        }
    }
    EXPECT_TRUE(raw_persisted) << "sanitizing must not rewrite persisted history";
}

// 场景：工具执行中保存自定义指令。期望本回合前缀不变，下一回合生效；
// 这是 D8 有意采用的回合级快照语义，防止配置替换导致中途串用新值。
TEST(AgentLoopTermination, CustomInstructionSaveAffectsOnlyTheNextTurn) {
    TempHomeGuard home("acecode-prompt-snapshot");
    AgentLoopHarness h(home.root().string());
    h.custom_instruction_update("SNAPSHOT_BEFORE")();
    ToolImpl tool = create_noop_tool();
    tool.definition.name = "save_instructions";
    tool.execute = [update = h.custom_instruction_update("SNAPSHOT_AFTER")](
        const std::string&, const acecode::ToolContext&) {
        update();
        return ToolResult{"saved", true};
    };
    h.register_tool(std::move(tool));
    h.push_tool_call("save_instructions", "{}");
    h.push_text("first complete");
    ASSERT_TRUE(h.submit_and_wait("save instructions"));
    EXPECT_EQ(h.prompt_captures(), 1);
    h.push_text("second complete");
    ASSERT_TRUE(h.submit_and_wait("use new instructions"));
    EXPECT_EQ(h.prompt_captures(), 2);
    const auto first = h.request_messages_for_turn(0);
    const auto continued = h.request_messages_for_turn(1);
    const auto next = h.request_messages_for_turn(2);
    ASSERT_FALSE(first.empty());
    ASSERT_GE(continued.size(), first.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].role, continued[i].role);
        EXPECT_EQ(first[i].content, continued[i].content);
    }
    auto contains = [](const auto& messages, const std::string& text) {
        return std::any_of(messages.begin(), messages.end(), [&](const auto& message) {
            return message.content.find(text) != std::string::npos;
        });
    };
    EXPECT_TRUE(contains(first, "SNAPSHOT_BEFORE"));
    EXPECT_FALSE(contains(continued, "SNAPSHOT_AFTER"));
    EXPECT_TRUE(contains(next, "SNAPSHOT_AFTER"));
    EXPECT_FALSE(contains(next, "SNAPSHOT_BEFORE"));
}

// 场景：会话空闲时从关闭记忆切换为开启。期望下一回合立即采用新值；
// 不再借用先前传入配置对象的地址，因此调用者配置离开作用域也不影响回合。
TEST(AgentLoopTermination, IdleMemoryConfigurationSaveAppearsInNextTurn) {
    TempHomeGuard home("acecode-memory-config-snapshot");
    auto memory = std::make_shared<acecode::MemoryService>(
        acecode::get_memory_dir(), acecode::get_memory_state_db_path(), acecode::MemoryConfig{});
    std::string error;
    ASSERT_TRUE(memory->global().upsert("snapshot_memory", acecode::MemoryType::User,
        "snapshot memory", "MEMORY_SNAPSHOT_CONTENT\n",
        acecode::MemoryWriteMode::Create, error).has_value()) << error;
    AgentLoopHarness h(home.root().string(), {}, memory);
    {
        acecode::MemoryConfig config;
        config.enabled = false;
        h.set_memory_config(&config);
    }
    h.push_text("first");
    ASSERT_TRUE(h.submit_and_wait("without memory"));
    {
        acecode::MemoryConfig config;
        config.enabled = true;
        h.set_memory_config(&config);
    }
    h.push_text("second");
    ASSERT_TRUE(h.submit_and_wait("with memory"));
    auto contains = [](const auto& messages) {
        return std::any_of(messages.begin(), messages.end(), [](const auto& message) {
            return message.content.find("snapshot_memory") != std::string::npos;
        });
    };
    EXPECT_FALSE(contains(h.request_messages_for_turn(0)));
    EXPECT_TRUE(contains(h.request_messages_for_turn(1)));
}
