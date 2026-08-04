// SessionChannelBinder 纯逻辑单测(openspec: daemon 托管 remote control)。
//
// 覆盖三块可测纯逻辑:
//   1. ChannelBindingState —— 绑定状态机:换绑覆盖旧绑定 + generation 过滤
//      (未绑定 session 的事件绝不允许流入 channel,Task 1 评审的硬约束)。
//   2. classify_session_event —— daemon 会话事件 → 出站动作的映射
//      (镜像 TUI 出站游标语义:只转发 assistant 文本与工具调用发起)。
//   3. KeepaliveDecider —— 连续失败阈值 + 60s 周期健康探测的再激活判定。
//
// 集成路径(service 起停 / 插件激活 / registry 订阅)在
// SessionChannelBinderIntegration 分组内用假 plugin runner + 真 registry 验证。

#include <gtest/gtest.h>

#include "remote_control/session_channel_binder.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using acecode::SessionEventKind;
using acecode::rc::ChannelBindingState;
using acecode::rc::KeepaliveDecider;
using acecode::rc::OutboundEventAction;
using acecode::rc::classify_session_event;
using acecode::rc::should_rebuild_binding;

// ---------- ChannelBindingState ----------

TEST(ChannelBindingState, StartsUnbound) {
    ChannelBindingState state;
    EXPECT_FALSE(state.bound());
    EXPECT_TRUE(state.bound_session().empty());
    EXPECT_FALSE(state.accepts("any", 0));
    EXPECT_FALSE(state.accepts("any", 1));
}

TEST(ChannelBindingState, BindSetsSessionAndBumpsGeneration) {
    ChannelBindingState state;
    const auto gen = state.bind("ses-a");
    EXPECT_TRUE(state.bound());
    EXPECT_EQ(state.bound_session(), "ses-a");
    EXPECT_EQ(state.generation(), gen);
    EXPECT_TRUE(state.accepts("ses-a", gen));
    // 同 session 但过期 generation(换绑前残留的订阅回调)也必须拒绝。
    EXPECT_FALSE(state.accepts("ses-a", gen + 1));
    EXPECT_FALSE(state.accepts("ses-b", gen));
}

TEST(ChannelBindingState, RebindReplacesOldBinding) {
    ChannelBindingState state;
    const auto gen_a = state.bind("ses-a");
    const auto gen_b = state.bind("ses-b");
    EXPECT_GT(gen_b, gen_a);
    EXPECT_EQ(state.bound_session(), "ses-b");
    // 旧绑定的 (session, generation) 组合全部失效 —— 未绑定会话的事件
    // 不允许泄漏进 channel。
    EXPECT_FALSE(state.accepts("ses-a", gen_a));
    EXPECT_FALSE(state.accepts("ses-a", gen_b));
    EXPECT_FALSE(state.accepts("ses-b", gen_a));
    EXPECT_TRUE(state.accepts("ses-b", gen_b));
}

TEST(ChannelBindingState, UnbindRejectsEverything) {
    ChannelBindingState state;
    const auto gen = state.bind("ses-a");
    state.unbind();
    EXPECT_FALSE(state.bound());
    EXPECT_TRUE(state.bound_session().empty());
    EXPECT_FALSE(state.accepts("ses-a", gen));
    // 重新 bind 后 generation 继续递增,旧代仍然拒绝。
    const auto gen2 = state.bind("ses-a");
    EXPECT_GT(gen2, gen);
    EXPECT_FALSE(state.accepts("ses-a", gen));
    EXPECT_TRUE(state.accepts("ses-a", gen2));
}

// ---------- should_rebuild_binding(启动重建条件,spec §五-3) ----------

TEST(ShouldRebuildBinding, EmptyBoundSessionNeverRebuilds) {
    EXPECT_FALSE(should_rebuild_binding("", true));
    EXPECT_FALSE(should_rebuild_binding("", false));
}

TEST(ShouldRebuildBinding, MissingSessionNeverRebuilds) {
    EXPECT_FALSE(should_rebuild_binding("ses-a", false));
}

TEST(ShouldRebuildBinding, RebuildsWhenBoundSessionExists) {
    EXPECT_TRUE(should_rebuild_binding("ses-a", true));
}

// ---------- classify_session_event ----------

TEST(ClassifySessionEvent, AssistantMessageForwardsText) {
    const auto action = classify_session_event(
        SessionEventKind::Message,
        {{"role", "assistant"}, {"content", "hello from agent"}, {"is_tool", false}});
    EXPECT_EQ(action.kind, OutboundEventAction::Kind::AssistantText);
    EXPECT_EQ(action.text, "hello from agent");
}

TEST(ClassifySessionEvent, IgnoresNonAssistantMessages) {
    const auto user = classify_session_event(
        SessionEventKind::Message,
        {{"role", "user"}, {"content", "hi"}, {"is_tool", false}});
    EXPECT_EQ(user.kind, OutboundEventAction::Kind::None);

    const auto system = classify_session_event(
        SessionEventKind::Message,
        {{"role", "system"}, {"content", "note"}, {"is_tool", false}});
    EXPECT_EQ(system.kind, OutboundEventAction::Kind::None);
}

TEST(ClassifySessionEvent, IgnoresToolAndBlankAssistantMessages) {
    const auto tool = classify_session_event(
        SessionEventKind::Message,
        {{"role", "assistant"}, {"content", "raw tool dump"}, {"is_tool", true}});
    EXPECT_EQ(tool.kind, OutboundEventAction::Kind::None);

    const auto blank = classify_session_event(
        SessionEventKind::Message,
        {{"role", "assistant"}, {"content", "  \n\t "}, {"is_tool", false}});
    EXPECT_EQ(blank.kind, OutboundEventAction::Kind::None);

    const auto missing = classify_session_event(
        SessionEventKind::Message, nlohmann::json{{"role", "assistant"}});
    EXPECT_EQ(missing.kind, OutboundEventAction::Kind::None);
}

// 需求④:普通工具调用一律抑制(不出站)。
TEST(ClassifySessionEvent, RegularToolStartSuppressed) {
    const auto action = classify_session_event(
        SessionEventKind::ToolStart,
        {{"tool", "bash"},
         {"args", {{"command", "git status"}}},
         {"command_preview", "git status"},
         {"is_task_complete", false}});
    EXPECT_EQ(action.kind, OutboundEventAction::Kind::None);

    // 缺 is_task_complete 字段的普通工具同样抑制。
    const auto no_flag = classify_session_event(
        SessionEventKind::ToolStart, nlohmann::json{{"tool", "bash"}});
    EXPECT_EQ(no_flag.kind, OutboundEventAction::Kind::None);

    const auto empty = classify_session_event(
        SessionEventKind::ToolStart, nlohmann::json::object());
    EXPECT_EQ(empty.kind, OutboundEventAction::Kind::None);
}

// 需求④:task_complete → 输出 args.summary 全文(作为 AssistantText)。
TEST(ClassifySessionEvent, TaskCompleteForwardsSummaryFullText) {
    const std::string summary = "已完成:改了 3 个文件并跑通全部测试。";
    // is_task_complete 布尔命中。
    const auto by_flag = classify_session_event(
        SessionEventKind::ToolStart,
        {{"tool", "task_complete"},
         {"is_task_complete", true},
         {"args", {{"summary", summary}}}});
    EXPECT_EQ(by_flag.kind, OutboundEventAction::Kind::AssistantText);
    EXPECT_EQ(by_flag.text, summary);

    // 仅靠 tool 名命中(无 is_task_complete 字段)也应识别。
    const auto by_name = classify_session_event(
        SessionEventKind::ToolStart,
        {{"tool", "task_complete"}, {"args", {{"summary", summary}}}});
    EXPECT_EQ(by_name.kind, OutboundEventAction::Kind::AssistantText);
    EXPECT_EQ(by_name.text, summary);

    // task_complete 但 summary 缺失/空白 → 不出站。
    const auto no_summary = classify_session_event(
        SessionEventKind::ToolStart,
        {{"tool", "task_complete"}, {"is_task_complete", true}, {"args", nlohmann::json::object()}});
    EXPECT_EQ(no_summary.kind, OutboundEventAction::Kind::None);
}

// 需求①:Error → reason 原样回传,按字符(码点)截断到 300。
TEST(ClassifySessionEvent, ErrorForwardsReasonTruncatedTo300Codepoints) {
    const auto short_err = classify_session_event(
        SessionEventKind::Error, {{"reason", "模型返回 400:鉴权失效"}});
    EXPECT_EQ(short_err.kind, OutboundEventAction::Kind::AssistantText);
    EXPECT_EQ(short_err.text, "模型返回 400:鉴权失效");

    // 350 个中文字符 → 截到 300 字符(码点),不加省略号。
    std::string long_reason;
    for (int i = 0; i < 350; ++i) long_reason += "错";
    const auto truncated = classify_session_event(
        SessionEventKind::Error, {{"reason", long_reason}});
    ASSERT_EQ(truncated.kind, OutboundEventAction::Kind::AssistantText);
    // 每个"错"是 3 字节;300 码点 = 900 字节。
    EXPECT_EQ(truncated.text.size(), 900u);
    std::string expected300;
    for (int i = 0; i < 300; ++i) expected300 += "错";
    EXPECT_EQ(truncated.text, expected300);

    // 恰好 300 不截。
    const auto exact = classify_session_event(
        SessionEventKind::Error, {{"reason", expected300}});
    EXPECT_EQ(exact.text, expected300);

    // 空白 reason 不出站。
    const auto blank = classify_session_event(
        SessionEventKind::Error, {{"reason", "   "}});
    EXPECT_EQ(blank.kind, OutboundEventAction::Kind::None);
}

TEST(ClassifySessionEvent, OtherEventKindsAreIgnored) {
    // Token/Done/Reasoning 等事件不触发任何 channel 出站;Error 与 ToolStart
    // 有各自出站规则,不在此列。
    for (auto kind : {SessionEventKind::Token, SessionEventKind::Reasoning,
                      SessionEventKind::ToolUpdate, SessionEventKind::ToolEnd,
                      SessionEventKind::BusyChanged, SessionEventKind::Done,
                      SessionEventKind::Usage}) {
        const auto action = classify_session_event(
            kind, {{"role", "assistant"}, {"content", "x"}, {"tool", "bash"}});
        EXPECT_EQ(action.kind, OutboundEventAction::Kind::None);
    }
}

// ---------- KeepaliveDecider ----------

TEST(KeepaliveDecider, TriggersAfterConsecutiveFailureThreshold) {
    KeepaliveDecider decider(/*failure_threshold=*/3, /*health_interval=*/60s);
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_TRUE(decider.on_outbound_result(false));
}

TEST(KeepaliveDecider, SuccessResetsFailureStreak) {
    KeepaliveDecider decider(3, 60s);
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_FALSE(decider.on_outbound_result(true));   // 成功清零
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_TRUE(decider.on_outbound_result(false));   // 重新数满 3 次才触发
}

TEST(KeepaliveDecider, TriggerResetsStreakToAvoidStorm) {
    KeepaliveDecider decider(3, 60s);
    decider.on_outbound_result(false);
    decider.on_outbound_result(false);
    EXPECT_TRUE(decider.on_outbound_result(false));
    // 触发后立刻再失败不应连发,要重新数满阈值。
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_TRUE(decider.on_outbound_result(false));
}

TEST(KeepaliveDecider, HealthProbeDueAfterInterval) {
    KeepaliveDecider decider(3, 60s);
    const auto t0 = KeepaliveDecider::Clock::now();
    decider.note_reactivated(t0);
    EXPECT_FALSE(decider.health_due(t0));
    EXPECT_FALSE(decider.health_due(t0 + 59s));
    EXPECT_TRUE(decider.health_due(t0 + 60s));
    EXPECT_TRUE(decider.health_due(t0 + 61s));
    EXPECT_EQ(decider.next_health_due(), t0 + 60s);

    decider.note_reactivated(t0 + 61s);
    EXPECT_FALSE(decider.health_due(t0 + 62s));
    EXPECT_EQ(decider.next_health_due(), t0 + 121s);
}

// ==================== 集成:binder 壳 ====================
//
// 假 plugin runner(不 spawn 进程)+ 真 SessionRegistry/LocalSessionClient +
// 真 RemoteControlService(loopback 测试端口),覆盖行为契约:
//   ② /rc 绑定 + 换绑覆盖 + bound_session_id/token 持久化
//   ③ 入站文本 → 绑定会话的 send_input(AgentLoop 提交路径)
//   ④ 只有绑定会话的 assistant/tool 事件出站(换绑后旧会话事件不得泄漏)
//   ⑤ 出站连续失败 ≥3 → 幂等再激活(runner 收到第二次 channel.activate)
//   ①/⑥ rebuild_from_config 重建 + shutdown 先停服务

#include "remote_control/remote_control_service.hpp"
#include "config/config.hpp"
#include "permissions.hpp"
#include "session/local_session_client.hpp"
#include "session/session_registry.hpp"
#include "tool/tool_executor.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

int next_binder_port() {
    static std::atomic<int> next{28611};
    return next.fetch_add(7);
}

#ifdef _WIN32
constexpr const char* kBinderHomeEnv = "USERPROFILE";
#else
constexpr const char* kBinderHomeEnv = "HOME";
#endif

class ScopedHome {
public:
    explicit ScopedHome(const fs::path& home) {
        if (const char* cur = std::getenv(kBinderHomeEnv)) {
            had_ = true;
            old_ = cur;
        }
        set(home.string());
    }
    ~ScopedHome() {
        if (had_) set(old_);
        else clear();
    }

private:
    static void set(const std::string& v) {
#ifdef _WIN32
        _putenv_s(kBinderHomeEnv, v.c_str());
#else
        setenv(kBinderHomeEnv, v.c_str(), 1);
#endif
    }
    static void clear() {
#ifdef _WIN32
        _putenv_s(kBinderHomeEnv, "");
#else
        unsetenv(kBinderHomeEnv);
#endif
    }
    bool had_ = false;
    std::string old_;
};

// 记录出站消息的假 sender(与 remote_control_hub_test 同款)。
class CaptureSender : public acecode::rc::OutboundSender {
public:
    bool send(const acecode::rc::OutboundMessage& msg, std::string*) override {
        std::lock_guard<std::mutex> lk(mu_);
        sent_.push_back(msg);
        cv_.notify_all();
        return succeed.load();
    }
    bool wait_for_count(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return sent_.size() >= n; });
    }
    std::vector<acecode::rc::OutboundMessage> sent() {
        std::lock_guard<std::mutex> lk(mu_);
        return sent_;
    }
    std::atomic<bool> succeed{true};

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<acecode::rc::OutboundMessage> sent_;
};

class ControlPublishBarrier {
public:
    ~ControlPublishBarrier() { release(); }

    void arm(std::string needle) {
        std::lock_guard<std::mutex> lk(mu_);
        needle_ = std::move(needle);
        entered_ = false;
        release_ = false;
        source_session_.clear();
    }

    void hook(const std::string& source_session, const std::string& text) {
        std::unique_lock<std::mutex> lk(mu_);
        if (needle_.empty() || text.find(needle_) == std::string::npos || entered_) {
            return;
        }
        source_session_ = source_session;
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lk, [&] { return release_; });
    }

    bool wait_until_entered(std::chrono::milliseconds timeout =
                                std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return entered_; });
    }

    void release() {
        std::lock_guard<std::mutex> lk(mu_);
        release_ = true;
        cv_.notify_all();
    }

    std::string source_session() const {
        std::lock_guard<std::mutex> lk(mu_);
        return source_session_;
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::string needle_;
    std::string source_session_;
    bool entered_ = false;
    bool release_ = false;
};

class InboundAcceptBarrier {
public:
    ~InboundAcceptBarrier() { release(); }

    void enter_and_wait() {
        std::unique_lock<std::mutex> lk(mu_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lk, [&] { return released_; });
    }

    bool wait_until_entered(std::chrono::milliseconds timeout =
                                std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return entered_; });
    }

    void release() {
        std::lock_guard<std::mutex> lk(mu_);
        released_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool released_ = false;
};

class InboundSuspendLatch {
public:
    void signal() {
        std::lock_guard<std::mutex> lk(mu_);
        signaled_ = true;
        cv_.notify_all();
    }

    bool wait(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return signaled_; });
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool signaled_ = false;
};

class ScopedInboundFenceHooks {
public:
    explicit ScopedInboundFenceHooks(acecode::rc::RemoteControlHub& hub)
        : hub_(hub) {}
    ~ScopedInboundFenceHooks() {
        hub_.set_inbound_fence_test_hooks({});
    }

private:
    acecode::rc::RemoteControlHub& hub_;
};

class MatchingUserMessageCounter {
public:
    explicit MatchingUserMessageCounter(std::string expected)
        : expected_(std::move(expected)) {}

    void observe(const acecode::SessionEvent& event) {
        if (event.kind != acecode::SessionEventKind::Message ||
            event.payload.value("role", std::string{}) != "user" ||
            event.payload.value("content", std::string{}) != expected_) {
            return;
        }
        std::lock_guard<std::mutex> lk(mu_);
        ++count_;
        cv_.notify_all();
    }

    bool wait_for(std::size_t count,
                  std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return count_ >= count; });
    }

    std::size_t count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return count_;
    }

private:
    std::string expected_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::size_t count_ = 0;
};

struct ThrowingConfigStore {
    acecode::AppConfig disk;
    bool throw_load = false;
    bool throw_save = false;

    acecode::AppConfig load() const {
        if (throw_load) throw std::runtime_error("injected config load failure");
        return disk;
    }

    void save(const acecode::AppConfig& next) {
        if (throw_save) throw std::runtime_error("injected config save failure");
        disk = next;
    }
};

// 假 plugin runner 的共享记录:激活/解绑请求 + 计数,支持带超时等待。
struct RunnerLog {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<nlohmann::json> activations;
    std::vector<nlohmann::json> deactivations;
    bool emit_binding_token = true;
    std::vector<std::string> activation_binding_tokens;
    std::size_t block_activation_number = 0;
    std::size_t fail_activation_number = 0;
    bool blocked_activation_entered = false;
    bool release_blocked_activation = false;
    bool fail_deactivation_with_request_echo = false;

    bool wait_for_activations(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [&] { return activations.size() >= n; });
    }

    bool wait_for_deactivations(std::size_t n,
                                std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [&] { return deactivations.size() >= n; });
    }

    bool wait_for_blocked_activation(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [&] {
            return blocked_activation_entered;
        });
    }

    void release_activation() {
        std::lock_guard<std::mutex> lk(mu);
        release_blocked_activation = true;
        cv.notify_all();
    }
};

acecode::rc::ChannelPluginHost::Runner make_fake_runner(
    std::shared_ptr<RunnerLog> log, std::string outbound_url) {
    return [log = std::move(log), outbound_url = std::move(outbound_url)](
               const acecode::HookCommandSpec&, const std::string& stdin_text,
               int, const std::string&) {
        acecode::HookProcessResult result;
        result.started = true;
        result.exit_code = 0;
        auto request = nlohmann::json::parse(stdin_text, nullptr, false);
        const std::string type =
            request.is_object() ? request.value("type", "") : "";
        std::string binding_token;
        bool fail_deactivation = false;
        bool fail_activation = false;
        {
            std::unique_lock<std::mutex> lk(log->mu);
            if (type == "channel.activate") {
                log->activations.push_back(request);
                const std::size_t activation_number = log->activations.size();
                if (log->emit_binding_token) {
                    binding_token =
                        activation_number <=
                                log->activation_binding_tokens.size()
                            ? log->activation_binding_tokens[
                                  activation_number - 1]
                            : "binding-" +
                                  std::to_string(activation_number);
                }
                if (activation_number == log->block_activation_number) {
                    log->blocked_activation_entered = true;
                    log->cv.notify_all();
                    log->cv.wait(lk, [&] {
                        return log->release_blocked_activation;
                    });
                }
                fail_activation = activation_number == log->fail_activation_number;
            }
            if (type == "channel.deactivate") {
                log->deactivations.push_back(request);
                fail_deactivation =
                    log->fail_deactivation_with_request_echo;
            }
            log->cv.notify_all();
        }
        if (type == "channel.activate") {
            nlohmann::json status{
                {"type", "channel.status"},
                {"state", fail_activation ? "failed" : "connected"},
                {"already_running", false},
                {"outbound",
                 nlohmann::json{{"mode", "webhook"}, {"url", outbound_url}}},
            };
            if (fail_activation) status["message"] = "injected activation failure";
            if (!binding_token.empty()) {
                status["binding_token"] = binding_token;
            }
            result.stdout_text = status.dump();
        } else if (fail_deactivation) {
            result.started = false;
            result.error =
                "deactivation request rejected: " + stdin_text;
        }
        return result;
    };
}

// 最小 daemon 侧脚手架:真 registry + client + service + binder。
struct BinderHarness {
    fs::path root;
    ScopedHome home;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AppConfig cfg;
    acecode::SessionRegistry registry;
    acecode::LocalSessionClient client;
    acecode::rc::RemoteControlService service;
    std::shared_ptr<RunnerLog> runner_log = std::make_shared<RunnerLog>();
    std::string config_path;

    explicit BinderHarness(const std::string& tag)
        : root(make_root(tag)), home(root / "home"), registry(make_deps(*this)),
          client(registry) {
        cfg.remote_control.port = next_binder_port();
        cfg.remote_control.default_channel = "chat";
        auto manifest_path = root / "channel-plugin.json";
        std::ofstream ofs(manifest_path);
        ofs << nlohmann::json{
            {"schema", "acecode.channel-plugin.v1"},
            {"name", "chat"},
            {"transport", "stdio"},
            {"launcher", nlohmann::json{{"command", "noop-channel-bridge"}}},
        }.dump();
        ofs.close();
        cfg.remote_control.channels["chat"].manifest_path =
            manifest_path.string();
        config_path = (root / "config.json").string();
    }

    ~BinderHarness() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static fs::path make_root(const std::string& tag) {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        auto root = fs::temp_directory_path() /
                    ("acecode-binder-" + tag + "-" + std::to_string(now));
        fs::create_directories(root / "home");
        return root;
    }

    std::string no_workspace_cache_root() const {
        return (root / "cache" / "no-workspace").string();
    }

    static acecode::SessionRegistryDeps make_deps(BinderHarness& self) {
        acecode::SessionRegistryDeps d;
        d.provider_accessor = [] {
            return std::shared_ptr<acecode::LlmProvider>();
        };
        d.tools = &self.tools;
        d.cwd = (self.root / "ws").string();
        d.no_workspace_cache_root = self.no_workspace_cache_root();
        d.config = nullptr;
        d.template_permissions = &self.permissions;
        return d;
    }

    acecode::rc::SessionChannelBinderDeps binder_deps(
        const std::string& outbound_url = "http://127.0.0.1:1/hook") {
        acecode::rc::SessionChannelBinderDeps d;
        d.service = &service;
        d.client = &client;
        d.config = &cfg;
        d.config_path = config_path;
        d.session_active = [this](const std::string& id) {
            return registry.acquire(id) != nullptr;
        };
        d.session_resumable = [this](const std::string& id) {
            // 与 worker.cpp 的生产接线一致:常规 resume + no-workspace 兜底。
            return acecode::rc::resume_session_with_no_workspace_fallback(
                client, id, no_workspace_cache_root());
        };
        d.resume_session_target = [this](const acecode::rc::RcSessionTarget& target) {
            return acecode::rc::resume_session_target_exact(client, target);
        };
        d.plugin_runner = make_fake_runner(runner_log, outbound_url);
        return d;
    }

    void emit_assistant(const std::string& id, const std::string& text) {
        auto* entry = registry.lookup(id);
        ASSERT_NE(entry, nullptr);
        entry->loop->events().emit(
            acecode::SessionEventKind::Message,
            {{"role", "assistant"}, {"content", text}, {"is_tool", false}});
    }

    void emit_tool_start(const std::string& id, const std::string& tool) {
        auto* entry = registry.lookup(id);
        ASSERT_NE(entry, nullptr);
        entry->loop->events().emit(
            acecode::SessionEventKind::ToolStart,
            {{"tool", tool}, {"args", {{"command", "echo hi"}}},
             {"is_task_complete", false}});
    }

    void emit_token(const std::string& id, const std::string& text) {
        auto* entry = registry.lookup(id);
        ASSERT_NE(entry, nullptr);
        entry->loop->events().emit(acecode::SessionEventKind::Token,
                                   {{"text", text}});
    }

    void emit_done(const std::string& id) {
        auto* entry = registry.lookup(id);
        ASSERT_NE(entry, nullptr);
        entry->loop->events().emit(acecode::SessionEventKind::Done,
                                   nlohmann::json::object());
    }

    void emit_task_complete(const std::string& id, const std::string& summary) {
        auto* entry = registry.lookup(id);
        ASSERT_NE(entry, nullptr);
        entry->loop->events().emit(
            acecode::SessionEventKind::ToolStart,
            {{"tool", "task_complete"},
             {"is_task_complete", true},
             {"args", {{"summary", summary}}}});
    }
};

nlohmann::json read_json_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return nlohmann::json();
    return nlohmann::json::parse(ifs, nullptr, false);
}

nlohmann::json channel_questions(std::size_t count = 2) {
    nlohmann::json questions = nlohmann::json::array({
        {
            {"id", "Choose the implementation?"},
            {"text", "Choose the implementation?"},
            {"header", "Implementation"},
            {"multiSelect", false},
            {"options", nlohmann::json::array({
                {{"label", "Alpha"}, {"value", "Alpha"},
                 {"description", "First approach"}},
                {{"label", "Beta"}, {"value", "Beta"},
                 {"description", "Second approach"}},
            })},
        },
        {
            {"id", "Add a note?"},
            {"text", "Add a note?"},
            {"header", "Note"},
            {"multiSelect", false},
            {"options", nlohmann::json::array({
                {{"label", "Yes"}, {"value", "Yes"},
                 {"description", "Include a note"}},
                {{"label", "No"}, {"value", "No"},
                 {"description", "Skip the note"}},
            })},
        },
    });
    while (questions.size() > count) questions.erase(questions.end() - 1);
    return questions;
}

bool wait_for_outbound_text(
    const std::shared_ptr<CaptureSender>& sender,
    const std::string& needle,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& message : sender->sent()) {
            if (message.text.find(needle) != std::string::npos) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (const auto& message : sender->sent()) {
        if (message.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::size_t outbound_index(const std::shared_ptr<CaptureSender>& sender,
                           const std::string& needle) {
    const auto messages = sender->sent();
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].text.find(needle) != std::string::npos) return i;
    }
    return messages.size();
}

} // namespace

TEST(SessionChannelBinderIntegration, BindRebindOffLifecycle) {
    BinderHarness hx("lifecycle");
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());

    const auto s1 = hx.client.create_session({});
    const auto s2 = hx.client.create_session({});

    // ② 绑定 s1:起服务 + 激活默认 channel + 持久化。
    auto bind1 = binder.execute_command(s1, "");
    ASSERT_TRUE(bind1.ok) << bind1.message;
    EXPECT_TRUE(hx.service.running());
    EXPECT_EQ(binder.bound_session_id(), s1);
    EXPECT_EQ(hx.cfg.remote_control.bound_session_id, s1);
    EXPECT_FALSE(hx.cfg.remote_control.token.empty());
    auto persisted = read_json_file(hx.config_path);
    ASSERT_TRUE(persisted.is_object());
    EXPECT_EQ(persisted["remote_control"]["bound_session_id"], s1);
    ASSERT_TRUE(hx.runner_log->wait_for_activations(1, std::chrono::seconds(5)));
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        const auto& req = hx.runner_log->activations.at(0);
        EXPECT_EQ(req["session_id"], s1);
        EXPECT_EQ(req["inbound"]["token"], hx.cfg.remote_control.token);
        EXPECT_NE(req["inbound"]["url"].get<std::string>().find("/rc/send"),
                  std::string::npos);
    }

    // ④ 出站:绑定会话的 assistant 文本出站,工具调用一律被抑制(需求④)。
    //    先换成捕获 sender(激活返回的 webhook url 不可达)。绑定确认消息
    //    (需求②)可能在换 sender 前已投给不可达 webhook,故按内容断言、不假设
    //    精确条数。
    auto has_text = [](const std::vector<acecode::rc::OutboundMessage>& v,
                       const std::string& t) {
        for (const auto& m : v) if (m.text == t) return true;
        return false;
    };
    auto has_tool_call = [](const std::vector<acecode::rc::OutboundMessage>& v) {
        for (const auto& m : v) if (m.type == "tool_call") return true;
        return false;
    };
    auto wait_until = [](auto pred, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return pred();
    };

    auto sender1 = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender1);
    hx.emit_assistant(s1, "bound reply");
    hx.emit_tool_start(s1, "bash");  // 抑制:不产生出站
    ASSERT_TRUE(wait_until([&] { return has_text(sender1->sent(), "bound reply"); },
                           std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(has_tool_call(sender1->sent()));  // 工具调用被抑制

    // ④ 反向:未绑定会话 s2 的事件绝不出站。
    hx.emit_assistant(s2, "must not leak");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(has_text(sender1->sent(), "must not leak"));

    // ③ 入站:channel 文本 → 绑定会话的提交路径(AgentLoop 收到该 user 输入)。
    {
        std::mutex mu;
        std::condition_variable cv;
        bool seen = false;
        auto sub = hx.client.subscribe(
            s1,
            [&](const acecode::SessionEvent& evt) {
                if (evt.kind != acecode::SessionEventKind::Message) return;
                if (!evt.payload.is_object()) return;
                if (evt.payload.value("role", "") != "user") return;
                if (evt.payload.value("content", "") != "inbound instruction") return;
                std::lock_guard<std::mutex> lk(mu);
                seen = true;
                cv.notify_all();
            },
            0);
        ASSERT_NE(sub, 0u);
        auto inbound = hx.service.hub().handle_inbound(
            "inbound instruction", hx.cfg.remote_control.token);
        EXPECT_TRUE(inbound.ok()) << inbound.message;
        {
            std::unique_lock<std::mutex> lk(mu);
            EXPECT_TRUE(cv.wait_for(lk, std::chrono::seconds(10),
                                    [&] { return seen; }));
        }
        hx.client.unsubscribe(s1, sub);
    }

    // ② 换绑 s2 覆盖旧绑定 + 持久化更新。
    auto bind2 = binder.execute_command(s2, "");
    ASSERT_TRUE(bind2.ok) << bind2.message;
    EXPECT_EQ(binder.bound_session_id(), s2);
    EXPECT_EQ(hx.cfg.remote_control.bound_session_id, s2);
    EXPECT_EQ(read_json_file(hx.config_path)["remote_control"]["bound_session_id"],
              s2);

    // ④ 换绑后:旧会话 s1 的事件不得泄漏,新会话 s2 正常出站。
    auto sender2 = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender2);
    hx.emit_assistant(s1, "stale session leak");
    hx.emit_assistant(s2, "rebound reply");
    ASSERT_TRUE(wait_until([&] { return has_text(sender2->sent(), "rebound reply"); },
                           std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(has_text(sender2->sent(), "stale session leak"));

    // off:解绑 + 停服务 + 清持久化 + 插件收到 deactivate。
    auto off = binder.execute_command(s2, "off");
    ASSERT_TRUE(off.ok) << off.message;
    EXPECT_FALSE(hx.service.running());
    EXPECT_TRUE(binder.bound_session_id().empty());
    EXPECT_TRUE(hx.cfg.remote_control.bound_session_id.empty());
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        ASSERT_EQ(hx.runner_log->deactivations.size(), 2u);
        EXPECT_EQ(hx.runner_log->deactivations.front()["session_id"], s1);
        EXPECT_EQ(hx.runner_log->deactivations.front()["binding_token"],
                  "binding-1");
        EXPECT_EQ(hx.runner_log->deactivations.back()["session_id"], s2);
        EXPECT_EQ(hx.runner_log->deactivations.back()["binding_token"],
                  "binding-2");
    }

    hx.registry.destroy(s1);
    hx.registry.destroy(s2);
}

TEST(SessionChannelBinderIntegration,
     LegacyBindingKeepsSessionOnlyDeactivateShape) {
    BinderHarness hx("legacy-token");
    hx.runner_log->emit_binding_token = false;
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    const auto sid = hx.client.create_session({});

    auto bind = binder.execute_command(sid, "");
    ASSERT_TRUE(bind.ok) << bind.message;
    auto off = binder.execute_command(sid, "off");
    ASSERT_TRUE(off.ok) << off.message;

    std::lock_guard<std::mutex> lk(hx.runner_log->mu);
    ASSERT_EQ(hx.runner_log->deactivations.size(), 1u);
    EXPECT_EQ(hx.runner_log->deactivations.front(),
              nlohmann::json({
                  {"type", "channel.deactivate"},
                  {"protocol_version", 1},
                  {"session_id", sid},
              }));
    hx.registry.destroy(sid);
}

TEST(SessionChannelBinderIntegration,
     DeactivationWarningRedactsBindingTokenFromRequestEcho) {
    BinderHarness hx("deactivate-error-redaction");
    const std::string token = R"(binding-"secret\line)";
    const std::string encoded = nlohmann::json(token).dump();
    const std::string escaped =
        encoded.substr(1, encoded.size() - 2);
    hx.runner_log->activation_binding_tokens = {token};
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    const auto sid = hx.client.create_session({});

    auto bind = binder.execute_command(sid, "");
    ASSERT_TRUE(bind.ok) << bind.message;
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        hx.runner_log->fail_deactivation_with_request_echo = true;
    }

    const auto off = binder.execute_command(sid, "off");
    ASSERT_TRUE(off.ok) << off.message;
    EXPECT_NE(off.message.find("Channel deactivate warning"),
              std::string::npos);
    EXPECT_NE(off.message.find("deactivation request rejected"),
              std::string::npos);
    EXPECT_NE(off.message.find("[binding token redacted]"),
              std::string::npos);
    EXPECT_EQ(off.message.find(token), std::string::npos)
        << off.message;
    EXPECT_EQ(off.message.find(escaped), std::string::npos)
        << off.message;
    hx.registry.destroy(sid);
}

TEST(SessionChannelBinderIntegration,
     SameSessionReactivationScopesStaleAndCurrentDeactivateTokens) {
    BinderHarness hx("same-session-token");
    hx.runner_log->activation_binding_tokens = {"token-A", "token-B"};
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    const auto sid = hx.client.create_session({});

    auto first = binder.execute_command(sid, "");
    ASSERT_TRUE(first.ok) << first.message;
    auto second = binder.execute_command(sid, "");
    ASSERT_TRUE(second.ok) << second.message;

    ASSERT_TRUE(hx.runner_log->wait_for_deactivations(
        1, std::chrono::seconds(5)));
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        ASSERT_EQ(hx.runner_log->deactivations.size(), 1u);
        EXPECT_EQ(hx.runner_log->deactivations.front()["session_id"], sid);
        EXPECT_EQ(hx.runner_log->deactivations.front()["binding_token"],
                  "token-A");
    }

    auto off = binder.execute_command(sid, "off");
    ASSERT_TRUE(off.ok) << off.message;
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        ASSERT_EQ(hx.runner_log->deactivations.size(), 2u);
        EXPECT_EQ(hx.runner_log->deactivations.back()["session_id"], sid);
        EXPECT_EQ(hx.runner_log->deactivations.back()["binding_token"],
                  "token-B");
    }
    hx.registry.destroy(sid);
}

TEST(SessionChannelBinderIntegration,
     SameSessionLegacyReactivationSkipsUnscopedStaleDeactivate) {
    BinderHarness hx("same-session-legacy");
    hx.runner_log->emit_binding_token = false;
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    const auto sid = hx.client.create_session({});

    ASSERT_TRUE(binder.execute_command(sid, "").ok);
    ASSERT_TRUE(binder.execute_command(sid, "").ok);
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        EXPECT_TRUE(hx.runner_log->deactivations.empty());
    }

    ASSERT_TRUE(binder.execute_command(sid, "off").ok);
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        ASSERT_EQ(hx.runner_log->deactivations.size(), 1u);
        EXPECT_FALSE(
            hx.runner_log->deactivations.front().contains("binding_token"));
    }
    hx.registry.destroy(sid);
}

TEST(SessionChannelBinderIntegration,
     ConcurrentReplaceAndOffUseTheBindingCurrentAfterReplacement) {
    BinderHarness hx("replace-off-race");
    hx.runner_log->activation_binding_tokens = {"token-A", "token-B"};
    hx.runner_log->block_activation_number = 2;
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    const auto sid = hx.client.create_session({});

    auto first = binder.execute_command(sid, "");
    ASSERT_TRUE(first.ok) << first.message;

    auto replacing = std::async(std::launch::async, [&] {
        return binder.execute_command(sid, "");
    });
    const bool replacement_blocked =
        hx.runner_log->wait_for_blocked_activation(std::chrono::seconds(5));
    if (!replacement_blocked) hx.runner_log->release_activation();
    ASSERT_TRUE(replacement_blocked);

    auto closing = std::async(std::launch::async, [&] {
        return binder.execute_command(sid, "off");
    });
    EXPECT_EQ(closing.wait_for(std::chrono::milliseconds(200)),
              std::future_status::timeout);

    hx.runner_log->release_activation();
    ASSERT_EQ(replacing.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    const auto replacement = replacing.get();
    ASSERT_TRUE(replacement.ok) << replacement.message;
    ASSERT_EQ(closing.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    const auto off = closing.get();
    ASSERT_TRUE(off.ok) << off.message;

    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        ASSERT_EQ(hx.runner_log->deactivations.size(), 2u);
        EXPECT_EQ(hx.runner_log->deactivations.front()["session_id"], sid);
        EXPECT_EQ(hx.runner_log->deactivations.front()["binding_token"],
                  "token-A");
        EXPECT_EQ(hx.runner_log->deactivations.back()["session_id"], sid);
        EXPECT_EQ(hx.runner_log->deactivations.back()["binding_token"],
                  "token-B");
    }
    EXPECT_TRUE(binder.bound_session_id().empty());
    hx.registry.destroy(sid);
}

TEST(SessionChannelBinderIntegration,
     ShutdownSerializesWithActivationWithoutDeactivatingPlugin) {
    BinderHarness hx("activate-shutdown-race");
    hx.runner_log->activation_binding_tokens = {"token-A"};
    hx.runner_log->block_activation_number = 1;
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    const auto sid = hx.client.create_session({});

    auto activating = std::async(std::launch::async, [&] {
        return binder.execute_command(sid, "");
    });
    const bool activation_blocked =
        hx.runner_log->wait_for_blocked_activation(std::chrono::seconds(5));
    if (!activation_blocked) hx.runner_log->release_activation();
    ASSERT_TRUE(activation_blocked);

    auto shutting_down = std::async(std::launch::async, [&] {
        binder.shutdown();
    });
    EXPECT_EQ(shutting_down.wait_for(std::chrono::milliseconds(200)),
              std::future_status::timeout);

    hx.runner_log->release_activation();
    ASSERT_EQ(activating.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    const auto activation = activating.get();
    ASSERT_TRUE(activation.ok) << activation.message;
    ASSERT_EQ(shutting_down.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    shutting_down.get();

    EXPECT_FALSE(hx.service.running());
    EXPECT_TRUE(binder.bound_session_id().empty());
    EXPECT_EQ(hx.cfg.remote_control.bound_session_id, sid);
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        EXPECT_TRUE(hx.runner_log->deactivations.empty());
    }
    const auto after_shutdown = binder.execute_command(sid, "");
    EXPECT_FALSE(after_shutdown.ok);
    EXPECT_NE(after_shutdown.message.find("shutting down"),
              std::string::npos);
    hx.registry.destroy(sid);
}

// 需求③④:固定确认已前移到 hub 合法入站路径,因此 Token/Done 不再触发或
// 复位提示;assistant 正文与 task_complete summary 仍出站,普通工具仍抑制。
TEST(SessionChannelBinderIntegration, TokenDoneDoNotTriggerHintAndTaskCompleteOutbound) {
    BinderHarness hx("thinking");
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());

    const auto s1 = hx.client.create_session({});
    auto bind1 = binder.execute_command(s1, "");
    ASSERT_TRUE(bind1.ok) << bind1.message;

    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);

    auto texts = [&] {
        std::vector<std::string> out;
        for (const auto& m : sender->sent()) out.push_back(m.text);
        return out;
    };
    auto count_text = [&](const std::string& t) {
        int n = 0;
        for (const auto& m : sender->sent()) if (m.text == t) ++n;
        return n;
    };
    auto wait_until = [](auto pred, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return pred();
    };

    // Token/Done 无论怎样组合都不产生"思考中..."。
    hx.emit_token(s1, "你");
    hx.emit_done(s1);
    hx.emit_token(s1, "好");
    hx.emit_done(s1);
    hx.emit_assistant(s1, "你好，我在。");
    // 等待同一事件流里后续 Message 已出站,即可确定此前 Token/Done 均已被
    // listener 消费,避免用固定 sleep 对异步调度做时序假设。
    ASSERT_TRUE(wait_until([&] {
        auto v = texts();
        return std::find(v.begin(), v.end(), "你好，我在。") != v.end();
    }, std::chrono::seconds(5)));
    EXPECT_EQ(count_text("思考中..."), 0);

    // task_complete → summary 全文出站;普通工具抑制。
    hx.emit_tool_start(s1, "bash");  // 抑制
    hx.emit_task_complete(s1, "已完成:回答了问候。");
    ASSERT_TRUE(wait_until([&] {
        auto v = texts();
        return std::find(v.begin(), v.end(), "已完成:回答了问候。") != v.end();
    }, std::chrono::seconds(5)));
    for (const auto& m : sender->sent()) {
        EXPECT_NE(m.type, "tool_call");  // 全程无 tool_call 出站
    }

    binder.execute_command(s1, "off");
    hx.registry.destroy(s1);
}

TEST(SessionChannelBinderIntegration, ConsecutiveOutboundFailuresTriggerReactivation) {
    BinderHarness hx("keepalive");
    auto deps = hx.binder_deps();
    deps.failure_threshold = 3;
    deps.health_interval = std::chrono::milliseconds(60000);  // 周期探测不参与本测
    acecode::rc::SessionChannelBinder binder(std::move(deps));

    const auto s1 = hx.client.create_session({});
    auto bind = binder.execute_command(s1, "");
    ASSERT_TRUE(bind.ok) << bind.message;
    ASSERT_TRUE(hx.runner_log->wait_for_activations(1, std::chrono::seconds(5)));

    auto entry = hx.registry.acquire(s1);
    ASSERT_TRUE(entry && entry->ask_prompter);
    auto initial_sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(initial_sender);
    auto pending = std::async(std::launch::async, [&] {
        return entry->ask_prompter->prompt(
            channel_questions(1), nullptr, std::chrono::seconds(30));
    });
    ASSERT_TRUE(wait_for_outbound_text(
        initial_sender, "Choose the implementation?"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto failed_before = hx.service.hub().stats().outbound_failed;

    // 连续 3 次出站失败 → 保活线程幂等重放 activate(⑤)。
    auto failing = std::make_shared<CaptureSender>();
    failing->succeed = false;
    hx.service.hub().set_outbound_sender(failing);
    hx.emit_assistant(s1, "fail 1");
    hx.emit_assistant(s1, "fail 2");
    hx.emit_assistant(s1, "fail 3");
    EXPECT_TRUE(hx.runner_log->wait_for_activations(2, std::chrono::seconds(10)));
    const auto reannounce_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (hx.service.hub().stats().outbound_failed < failed_before + 4 &&
           std::chrono::steady_clock::now() < reannounce_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_GE(
        hx.service.hub().stats().outbound_failed, failed_before + 4)
        << "3 条显式失败之外，恢复成功后还应重发 1 条当前问题";

    const auto snapshot = hx.client.snapshot_pending_questions(s1);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->size(), 1u);
    acecode::AskUserQuestionResponse cancelled;
    cancelled.cancelled = true;
    EXPECT_EQ(
        hx.client.respond_question(
            s1, snapshot->front().request_id, cancelled),
        acecode::QuestionResponseStatus::Accepted);
    ASSERT_EQ(pending.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    (void)pending.get();

    auto off = binder.execute_command(s1, "off");
    ASSERT_TRUE(off.ok) << off.message;
    EXPECT_FALSE(hx.service.running());
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        ASSERT_EQ(hx.runner_log->deactivations.size(), 2u);
        EXPECT_EQ(hx.runner_log->deactivations.front()["binding_token"],
                  "binding-1");
        EXPECT_EQ(hx.runner_log->deactivations.back()["binding_token"],
                  "binding-2");
    }
    hx.registry.destroy(s1);
}

TEST(SessionChannelBinderIntegration, RebuildFromConfigRestoresBinding) {
    BinderHarness hx("rebuild");
    const auto s1 = hx.client.create_session({});

    // ① bound_session_id 非空且会话存在 → 自动起服务 + 激活 + 重建绑定。
    hx.cfg.remote_control.bound_session_id = s1;
    {
        acecode::rc::SessionChannelBinder binder(hx.binder_deps());
        binder.rebuild_from_config();
        EXPECT_TRUE(hx.service.running());
        EXPECT_EQ(binder.bound_session_id(), s1);
        ASSERT_TRUE(hx.runner_log->wait_for_activations(1, std::chrono::seconds(5)));
        {
            std::lock_guard<std::mutex> lk(hx.runner_log->mu);
            EXPECT_EQ(hx.runner_log->activations.at(0)["session_id"], s1);
        }
        // ⑥ shutdown 先停服务;绑定持久化保留(下次启动继续重建)。
        binder.shutdown();
        EXPECT_FALSE(hx.service.running());
        EXPECT_EQ(hx.cfg.remote_control.bound_session_id, s1);
    }

    // ① 反向:bound 会话不存在 → 不起服务。
    {
        BinderHarness hx2("rebuild-miss");
        hx2.cfg.remote_control.bound_session_id = "ses-not-there";
        acecode::rc::SessionChannelBinder binder(hx2.binder_deps());
        binder.rebuild_from_config();
        EXPECT_FALSE(hx2.service.running());
        EXPECT_TRUE(binder.bound_session_id().empty());
    }

    hx.registry.destroy(s1);
}

// 回归:no_workspace 会话在 daemon 重启后的 resumable 探测。
// 触发场景:/rc 绑定的是「不使用工作区」会话,daemon 重启后启动重建按
// bound_session_id 探测该会话能否从磁盘恢复。
// bug 表现(修复前):探测只调 resume_session(id) 默认 SessionOptions,
// with_resolved_workspace 把 cwd 解析成 daemon 自身 cwd,而 no_workspace
// 会话的 meta 落在 cache/no-workspace/<id>/ 对应的项目目录下,永远 miss →
// 日志 "bound session <id> not found; skipping channel binding rebuild",
// 需要手动重跑 /rc 才能恢复绑定。
TEST(SessionChannelBinderIntegration, NoWorkspaceSessionResumableAfterRestart) {
    BinderHarness hx("no-ws-resume");
    acecode::SessionOptions no_ws;
    no_ws.no_workspace = true;
    const auto sid = hx.client.create_session(no_ws);
    ASSERT_FALSE(sid.empty());
    {
        // meta 是首次落盘才写的(lazy);真实场景绑定过 /rc 的会话必然有过
        // 落盘,这里用 ensure_active_session_id 强制写出 initial meta。
        auto* entry = hx.registry.lookup(sid);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->sm->ensure_active_session_id(), sid);
    }
    // 模拟 daemon 重启:内存 registry 清空,磁盘数据保留。
    hx.registry.destroy(sid);
    ASSERT_EQ(hx.registry.lookup(sid), nullptr);

    // 常规 resume(默认 SessionOptions)找不到 no_workspace 会话 —— 这条
    // 断言钉住 bug 的直接根因;若未来常规 resume 自己学会兜底,这里会失败,
    // 提醒同步简化 resume_session_with_no_workspace_fallback。
    EXPECT_FALSE(hx.client.resume_session(sid));

    // 兜底探测应命中缓存目录里的 meta 并以 no_workspace 选项恢复会话。
    EXPECT_TRUE(acecode::rc::resume_session_with_no_workspace_fallback(
        hx.client, sid, hx.no_workspace_cache_root()));
    auto* resumed = hx.registry.lookup(sid);
    ASSERT_NE(resumed, nullptr);
    EXPECT_TRUE(resumed->no_workspace);

    hx.registry.destroy(sid);
}

// 回归:binder 对共享 AppConfig 的读写与 persist 落盘必须走注入的
// with_config_lock(生产接线 = WebServer::Impl::app_config_mu)。修复前
// binder 在 Crow HTTP 线程上裸读写 cfg_mut 并整份 save_config —— 与
// config PUT / 连接器钩子并发即数据竞争 + config.json 交错写坏。
TEST(SessionChannelBinderIntegration, PersistPathRunsUnderInjectedConfigLock) {
    BinderHarness hx("lock-double");
    auto deps = hx.binder_deps();

    struct LockProbe {
        std::atomic<int> uses{0};
        std::atomic<int> depth{0};
        std::atomic<int> reloads{0};
        std::atomic<int> reloads_outside_lock{0};
    };
    auto probe = std::make_shared<LockProbe>();
    deps.with_config_lock = [probe](const std::function<void()>& fn) {
        probe->uses.fetch_add(1);
        probe->depth.fetch_add(1);
        fn();
        probe->depth.fetch_sub(1);
    };
    deps.load_disk_config = [probe] {
        probe->reloads.fetch_add(1);
        if (probe->depth.load() <= 0) probe->reloads_outside_lock.fetch_add(1);
        return acecode::AppConfig{};
    };
    acecode::rc::SessionChannelBinder binder(std::move(deps));

    const auto s1 = hx.client.create_session({});
    auto bind = binder.execute_command(s1, "");
    ASSERT_TRUE(bind.ok) << bind.message;

    // bind 期间的 config 读取 + persist 的 reload-merge-save 都在锁回调内。
    EXPECT_GE(probe->uses.load(), 2);
    EXPECT_GE(probe->reloads.load(), 1);
    EXPECT_EQ(probe->reloads_outside_lock.load(), 0);
    // 落盘的是 merge 后副本:binder 拥有的字段进了磁盘。
    auto persisted = read_json_file(hx.config_path);
    ASSERT_TRUE(persisted.is_object());
    EXPECT_EQ(persisted["remote_control"]["bound_session_id"], s1);

    binder.shutdown();
    hx.registry.destroy(s1);
}

// 回归:persist_binding 不得用 stale 内存快照整份覆盖磁盘。场景:连接器
// 钩子在 bind 与下一次 persist 之间把 api_key 直写 config.json(生产中
// 钩子预算长达数分钟,重叠窗口真实存在);binder 的内存 config 不知情。
// 修复前:第二次 persist 整份序列化内存 config → 磁盘上刚写入的
// saved_models api_key 被抹掉。修复后:persist 先重读磁盘,只 merge
// binder 拥有的 remote_control.bound_session_id / token,再落盘。
TEST(SessionChannelBinderIntegration, PersistBindingDoesNotClobberOtherWritersDiskState) {
    BinderHarness hx("clobber");
    auto deps = hx.binder_deps();
    // 镜像生产接线:落盘走默认路径(scoped HOME 下的 ~/.acecode/config.json),
    // reload 兜底 acecode::load_config() 读的是同一份文件。
    deps.config_path.clear();
    acecode::rc::SessionChannelBinder binder(std::move(deps));

    const auto s1 = hx.client.create_session({});
    const auto s2 = hx.client.create_session({});
    auto bind1 = binder.execute_command(s1, "");
    ASSERT_TRUE(bind1.ok) << bind1.message;

    // 另一写方(连接器钩子)把新 api_key 落盘;只写磁盘,不碰 binder 内存。
    {
        acecode::AppConfig other = acecode::load_config();
        acecode::ModelProfile profile;
        profile.name = "connector-managed";
        profile.provider = "openai";
        profile.base_url = "http://127.0.0.1:9/v1";
        profile.api_key = "fresh-key-from-hook";
        profile.model = "demo";
        other.saved_models.push_back(profile);
        acecode::save_config(other);
    }

    // 换绑触发第二次 persist —— 修复前这里会把磁盘上的 api_key 清掉。
    auto bind2 = binder.execute_command(s2, "");
    ASSERT_TRUE(bind2.ok) << bind2.message;

    const std::string default_path =
        (fs::path(acecode::get_acecode_dir()) / "config.json").string();
    auto disk = read_json_file(default_path);
    ASSERT_TRUE(disk.is_object());
    EXPECT_EQ(disk["remote_control"]["bound_session_id"], s2);
    ASSERT_TRUE(disk.contains("saved_models") && disk["saved_models"].is_array());
    bool preserved = false;
    for (const auto& m : disk["saved_models"]) {
        if (m.value("name", "") == "connector-managed" &&
            m.value("api_key", "") == "fresh-key-from-hook") {
            preserved = true;
        }
    }
    EXPECT_TRUE(preserved)
        << "another writer's on-disk saved_models entry was clobbered by "
           "persist_binding: " << disk.dump(2);

    binder.shutdown();
    hx.registry.destroy(s1);
    hx.registry.destroy(s2);
}

// 回归:rebuild_from_config 对 no_workspace 绑定会话的完整重启重建链路
//(harness 的 session_resumable 与 worker.cpp 生产接线一致)。
// 期望:服务拉起 + 绑定恢复为原会话 + 激活请求携带原会话 id。
// 修复前:session_resumable 返回 false → should_rebuild_binding 不成立 →
// WARN 跳过,服务不启动、绑定为空(配置不脏写,但远程通道静默失联)。
TEST(SessionChannelBinderIntegration, RebuildFromConfigRestoresNoWorkspaceBinding) {
    BinderHarness hx("rebuild-no-ws");
    acecode::SessionOptions no_ws;
    no_ws.no_workspace = true;
    const auto sid = hx.client.create_session(no_ws);
    ASSERT_FALSE(sid.empty());
    {
        auto* entry = hx.registry.lookup(sid);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->sm->ensure_active_session_id(), sid);
    }
    hx.cfg.remote_control.bound_session_id = sid;
    // 模拟 daemon 重启:会话只剩磁盘数据,重建全靠 resumable 探测。
    hx.registry.destroy(sid);

    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    binder.rebuild_from_config();
    EXPECT_TRUE(hx.service.running());
    EXPECT_EQ(binder.bound_session_id(), sid);
    ASSERT_TRUE(hx.runner_log->wait_for_activations(1, std::chrono::seconds(5)));
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        EXPECT_EQ(hx.runner_log->activations.at(0)["session_id"], sid);
    }
    binder.shutdown();
    hx.registry.destroy(sid);
}

TEST(SessionChannelBinderIntegration,
     MultiQuestionChannelDraftUnblocksOnlyAfterFinalAnswer) {
    BinderHarness hx("channel-question");
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    const auto sid = hx.client.create_session({});
    auto bind = binder.execute_command(sid, "");
    ASSERT_TRUE(bind.ok) << bind.message;

    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);
    std::atomic<int> aq_user_messages{0};
    const auto transcript_sub = hx.client.subscribe(
        sid,
        [&](const acecode::SessionEvent& event) {
            if (event.kind == acecode::SessionEventKind::Message &&
                event.payload.value("role", std::string{}) == "user" &&
                event.payload.value("content", std::string{}).find("/aq") !=
                    std::string::npos) {
                ++aq_user_messages;
            }
        });
    ASSERT_NE(transcript_sub, 0u);

    auto entry = hx.registry.acquire(sid);
    ASSERT_TRUE(entry && entry->ask_prompter);
    auto pending = std::async(std::launch::async, [&] {
        return entry->ask_prompter->prompt(
            channel_questions(), nullptr, std::chrono::seconds(5));
    });
    ASSERT_TRUE(wait_for_outbound_text(sender, "Choose the implementation?"));

    const auto first = hx.service.hub().handle_inbound(
        "/aq 1", hx.cfg.remote_control.token);
    ASSERT_TRUE(first.ok()) << first.message;
    EXPECT_EQ(pending.wait_for(std::chrono::milliseconds(200)),
              std::future_status::timeout);
    ASSERT_TRUE(wait_for_outbound_text(sender, "已记录第 1/2 题"));
    ASSERT_TRUE(wait_for_outbound_text(sender, "Add a note?"));

    const auto final = hx.service.hub().handle_inbound(
        "/aq deployment note", hx.cfg.remote_control.token);
    ASSERT_TRUE(final.ok()) << final.message;
    ASSERT_EQ(pending.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    const auto response = pending.get();
    EXPECT_FALSE(response.cancelled);
    EXPECT_FALSE(response.timed_out);
    ASSERT_EQ(response.answers.size(), 2u);
    EXPECT_EQ(response.answers[0].selected,
              std::vector<std::string>({"Alpha"}));
    EXPECT_EQ(response.answers[1].custom_text, "deployment note");
    EXPECT_TRUE(wait_for_outbound_text(sender, "答案已提交，继续执行"));
    EXPECT_EQ(aq_user_messages.load(), 0);

    const auto thinking = outbound_index(sender, "思考中...");
    const auto recorded = outbound_index(sender, "已记录第 1/2 题");
    EXPECT_LT(thinking, recorded)
        << "Hub acknowledgement must remain ahead of bridge output";

    hx.client.unsubscribe(sid, transcript_sub);
    binder.execute_command(sid, "off");
    hx.registry.destroy(sid);
}

TEST(SessionChannelBinderIntegration,
     WebAnswerWinsAndAuthoritativeCloseClearsChannelDraft) {
    BinderHarness hx("question-web-first");
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    const auto sid = hx.client.create_session({});
    auto bind = binder.execute_command(sid, "");
    ASSERT_TRUE(bind.ok) << bind.message;
    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);

    auto entry = hx.registry.acquire(sid);
    ASSERT_TRUE(entry && entry->ask_prompter);
    auto pending = std::async(std::launch::async, [&] {
        return entry->ask_prompter->prompt(
            channel_questions(), nullptr, std::chrono::seconds(5));
    });
    ASSERT_TRUE(wait_for_outbound_text(sender, "Choose the implementation?"));
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/aq 1", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(wait_for_outbound_text(sender, "Add a note?"));

    const auto snapshot = hx.client.snapshot_pending_questions(sid);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->size(), 1u);
    acecode::AskUserQuestionResponse web_response;
    acecode::AskUserQuestionAnswer first_answer;
    first_answer.question_id = "Choose the implementation?";
    first_answer.selected = {"Beta"};
    web_response.answers.push_back(std::move(first_answer));
    acecode::AskUserQuestionAnswer second_answer;
    second_answer.question_id = "Add a note?";
    second_answer.selected = {"No"};
    web_response.answers.push_back(std::move(second_answer));
    EXPECT_EQ(
        hx.client.respond_question(
            sid, snapshot->front().request_id, web_response),
        acecode::QuestionResponseStatus::Accepted);

    ASSERT_EQ(pending.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    const auto response = pending.get();
    ASSERT_EQ(response.answers.size(), 2u);
    EXPECT_EQ(response.answers[0].selected,
              std::vector<std::string>({"Beta"}));
    EXPECT_TRUE(wait_for_outbound_text(
        sender, "问题已在 ACECode 页面完成，本端草稿已清除"));

    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/aq 2", hx.cfg.remote_control.token).ok());
    EXPECT_TRUE(wait_for_outbound_text(sender, "当前没有待回答的问题"));

    binder.execute_command(sid, "off");
    hx.registry.destroy(sid);
}

TEST(SessionChannelBinderIntegration,
     ConcurrentWebAndChannelAnswersKeepPrompterFirstWins) {
    BinderHarness hx("question-race");
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    const auto sid = hx.client.create_session({});
    auto bind = binder.execute_command(sid, "");
    ASSERT_TRUE(bind.ok) << bind.message;
    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);

    auto entry = hx.registry.acquire(sid);
    ASSERT_TRUE(entry && entry->ask_prompter);
    auto pending = std::async(std::launch::async, [&] {
        return entry->ask_prompter->prompt(
            channel_questions(1), nullptr, std::chrono::seconds(5));
    });
    ASSERT_TRUE(wait_for_outbound_text(sender, "Choose the implementation?"));
    const auto snapshot = hx.client.snapshot_pending_questions(sid);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->size(), 1u);

    acecode::AskUserQuestionResponse web_response;
    acecode::AskUserQuestionAnswer web_answer;
    web_answer.question_id = "Choose the implementation?";
    web_answer.selected = {"Beta"};
    web_response.answers.push_back(std::move(web_answer));

    std::atomic<bool> start{false};
    acecode::QuestionResponseStatus web_status =
        acecode::QuestionResponseStatus::Closed;
    acecode::rc::InboundResult channel_status;
    std::thread web([&] {
        while (!start.load()) std::this_thread::yield();
        web_status = hx.client.respond_question(
            sid, snapshot->front().request_id, web_response);
    });
    std::thread channel([&] {
        while (!start.load()) std::this_thread::yield();
        channel_status = hx.service.hub().handle_inbound(
            "/aq 1", hx.cfg.remote_control.token);
    });
    start.store(true);
    web.join();
    channel.join();

    EXPECT_TRUE(channel_status.ok()) << channel_status.message;
    ASSERT_EQ(pending.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    const auto response = pending.get();
    ASSERT_EQ(response.answers.size(), 1u);
    ASSERT_EQ(response.answers[0].selected.size(), 1u);
    if (web_status == acecode::QuestionResponseStatus::Accepted) {
        EXPECT_EQ(response.answers[0].selected[0], "Beta");
    } else {
        EXPECT_EQ(web_status, acecode::QuestionResponseStatus::Closed);
        EXPECT_EQ(response.answers[0].selected[0], "Alpha");
    }

    binder.execute_command(sid, "off");
    hx.registry.destroy(sid);
}

TEST(SessionChannelBinderIntegration,
     RebindGenerationRejectsOldAnswersAndSnapshotRestoresPendingBatch) {
    BinderHarness hx("question-rebind");
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    const auto first_sid = hx.client.create_session({});
    const auto second_sid = hx.client.create_session({});
    auto first_bind = binder.execute_command(first_sid, "");
    ASSERT_TRUE(first_bind.ok) << first_bind.message;
    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);

    auto entry = hx.registry.acquire(first_sid);
    ASSERT_TRUE(entry && entry->ask_prompter);
    auto pending = std::async(std::launch::async, [&] {
        return entry->ask_prompter->prompt(
            channel_questions(), nullptr, std::chrono::seconds(10));
    });
    ASSERT_TRUE(wait_for_outbound_text(sender, "Choose the implementation?"));
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/aq 1", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(wait_for_outbound_text(sender, "Add a note?"));

    auto second_bind = binder.execute_command(second_sid, "");
    ASSERT_TRUE(second_bind.ok) << second_bind.message;
    sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/aq 1", hx.cfg.remote_control.token).ok());
    EXPECT_EQ(pending.wait_for(std::chrono::milliseconds(200)),
              std::future_status::timeout);
    EXPECT_TRUE(wait_for_outbound_text(sender, "当前没有待回答的问题"));

    auto rebound = binder.execute_command(first_sid, "");
    ASSERT_TRUE(rebound.ok) << rebound.message;
    sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/aq --status", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(wait_for_outbound_text(sender, "第 1/2 题"));
    EXPECT_TRUE(wait_for_outbound_text(sender, "已记录 0/2 题"));

    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/aq 2", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/aq restored", hx.cfg.remote_control.token).ok());
    ASSERT_EQ(pending.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    const auto response = pending.get();
    ASSERT_EQ(response.answers.size(), 2u);
    EXPECT_EQ(response.answers[0].selected,
              std::vector<std::string>({"Beta"}));
    EXPECT_EQ(response.answers[1].custom_text, "restored");

    binder.execute_command(first_sid, "off");
    hx.registry.destroy(first_sid);
    hx.registry.destroy(second_sid);
}

TEST(SessionChannelBinderIntegration,
     RcSessionCommandsAreConsumedAndShareStableSnapshotAcrossAliases) {
    BinderHarness hx("session-command-snapshot");
    const auto first = hx.client.create_session({});
    const auto second = hx.client.create_session({});
    std::vector<acecode::rc::RcSessionTarget> catalog;
    catalog.push_back({first, "workspace-a", "C:/workspace-a", "first", "", "A",
                       "2026-08-01T01:00:00Z", false, true, 0});
    catalog.push_back({second, "workspace-b", "C:/workspace-b", "second", "", "B",
                       "2026-08-02T01:00:00Z", false, true, 0});
    std::mutex selected_mu;
    std::vector<acecode::rc::RcSessionTarget> selected;

    auto deps = hx.binder_deps();
    deps.session_catalog = [&catalog](const std::optional<std::string>&) { return catalog; };
    deps.on_session_selected = [&selected_mu, &selected](const acecode::rc::RcSessionTarget& target) {
        std::lock_guard<std::mutex> lk(selected_mu);
        selected.push_back(target);
    };
    acecode::rc::SessionChannelBinder binder(std::move(deps));
    ASSERT_TRUE(binder.execute_command(first, "").ok);
    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);

    std::atomic<bool> command_reached_agent{false};
    const auto sub = hx.client.subscribe(first, [&](const acecode::SessionEvent& event) {
        if (event.kind == acecode::SessionEventKind::Message &&
            event.payload.value("role", std::string{}) == "user" &&
            event.payload.value("content", std::string{}) == "/sessions") {
            command_reached_agent.store(true);
        }
    });
    ASSERT_NE(sub, 0u);
    ASSERT_TRUE(hx.service.hub().handle_inbound("/sessions", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(wait_for_outbound_text(sender, "Recent sessions:"));
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(command_reached_agent.load());

    ASSERT_TRUE(hx.service.hub().handle_inbound("/resume 1", hx.cfg.remote_control.token).ok());
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while ([&] {
               std::lock_guard<std::mutex> lk(selected_mu);
               return selected.empty();
           }() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    {
        std::lock_guard<std::mutex> lk(selected_mu);
        ASSERT_EQ(selected.size(), 1u);
        EXPECT_EQ(selected.front().session_id, second);
    }
    EXPECT_EQ(binder.bound_session_id(), second);
    hx.client.unsubscribe(first, sub);
    binder.execute_command(second, "off");
    hx.registry.destroy(first);
    hx.registry.destroy(second);
}

TEST(SessionChannelBinderIntegration,
     ExactTargetResumeRestoresCrossWorkspaceAndNoWorkspaceIdentities) {
    BinderHarness hx("session-target-resume");
    const auto find_info = [&hx](const std::string& id) {
        for (const auto& info : hx.client.list_sessions()) {
            if (info.id == id) return info;
        }
        return acecode::SessionInfo{};
    };

    const auto other_cwd = (hx.root / "other-workspace").string();
    fs::create_directories(other_cwd);
    acecode::SessionOptions cross_workspace;
    cross_workspace.cwd = other_cwd;
    const auto cross_id = hx.client.create_session(cross_workspace);
    const auto cross_info = find_info(cross_id);
    ASSERT_FALSE(cross_info.id.empty());
    auto* cross_entry = hx.registry.lookup(cross_id);
    ASSERT_NE(cross_entry, nullptr);
    ASSERT_EQ(cross_entry->sm->ensure_active_session_id(), cross_id);
    hx.registry.destroy(cross_id);
    acecode::rc::RcSessionTarget cross_target;
    cross_target.session_id = cross_id;
    cross_target.cwd = cross_info.cwd;
    cross_target.workspace_hash = cross_info.workspace_hash;
    ASSERT_TRUE(acecode::rc::resume_session_target_exact(hx.client, cross_target));
    const auto restored_cross = hx.registry.acquire(cross_id);
    ASSERT_NE(restored_cross, nullptr);
    EXPECT_FALSE(restored_cross->no_workspace);
    EXPECT_EQ(restored_cross->cwd, cross_info.cwd);
    EXPECT_EQ(restored_cross->workspace_hash, cross_info.workspace_hash);

    acecode::SessionOptions no_workspace;
    no_workspace.no_workspace = true;
    const auto no_workspace_id = hx.client.create_session(no_workspace);
    const auto no_workspace_info = find_info(no_workspace_id);
    ASSERT_FALSE(no_workspace_info.id.empty());
    auto* no_workspace_entry = hx.registry.lookup(no_workspace_id);
    ASSERT_NE(no_workspace_entry, nullptr);
    ASSERT_EQ(no_workspace_entry->sm->ensure_active_session_id(), no_workspace_id);
    hx.registry.destroy(no_workspace_id);
    acecode::rc::RcSessionTarget no_workspace_target;
    no_workspace_target.session_id = no_workspace_id;
    no_workspace_target.cwd = no_workspace_info.cwd;
    no_workspace_target.no_workspace = true;
    ASSERT_TRUE(acecode::rc::resume_session_target_exact(hx.client, no_workspace_target));
    const auto restored_no_workspace = hx.registry.acquire(no_workspace_id);
    ASSERT_NE(restored_no_workspace, nullptr);
    EXPECT_TRUE(restored_no_workspace->no_workspace);
    EXPECT_EQ(restored_no_workspace->cwd, no_workspace_info.cwd);
    EXPECT_TRUE(restored_no_workspace->workspace_hash.empty());

    hx.registry.destroy(cross_id);
    hx.registry.destroy(no_workspace_id);
}

TEST(SessionChannelBinderIntegration,
     RcSessionCatalogWorkDoesNotBlockInboundCallback) {
    BinderHarness hx("session-command-nonblocking");
    const auto session = hx.client.create_session({});
    std::mutex catalog_mu;
    std::condition_variable catalog_cv;
    bool entered = false;
    bool release = false;
    auto deps = hx.binder_deps();
    deps.session_catalog = [&](const std::optional<std::string>&) {
        std::unique_lock<std::mutex> lk(catalog_mu);
        entered = true;
        catalog_cv.notify_all();
        catalog_cv.wait(lk, [&] { return release; });
        return std::vector<acecode::rc::RcSessionTarget>{};
    };
    acecode::rc::SessionChannelBinder binder(std::move(deps));
    ASSERT_TRUE(binder.execute_command(session, "").ok);
    const auto before = std::chrono::steady_clock::now();
    ASSERT_TRUE(hx.service.hub().handle_inbound("/session", hx.cfg.remote_control.token).ok());
    const auto elapsed = std::chrono::steady_clock::now() - before;
    EXPECT_LT(elapsed, 100ms);
    {
        std::unique_lock<std::mutex> lk(catalog_mu);
        ASSERT_TRUE(catalog_cv.wait_for(lk, 3s, [&] { return entered; }));
        release = true;
    }
    catalog_cv.notify_all();
    binder.execute_command(session, "off");
    hx.registry.destroy(session);
}

TEST(SessionChannelBinderIntegration,
     FailedReplacementKeepsOldBindingUsable) {
    BinderHarness hx("replacement-rollback");
    const auto first = hx.client.create_session({});
    const auto second = hx.client.create_session({});
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    ASSERT_TRUE(binder.execute_command(first, "").ok);
    hx.runner_log->fail_activation_number = 2;
    const auto failed = binder.execute_command(second, "");
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(binder.bound_session_id(), first);
    EXPECT_TRUE(hx.service.running());

    std::atomic<bool> delivered{false};
    const auto sub = hx.client.subscribe(first, [&](const acecode::SessionEvent& event) {
        if (event.kind == acecode::SessionEventKind::Message &&
            event.payload.value("role", std::string{}) == "user" &&
            event.payload.value("content", std::string{}) == "still routed") {
            delivered.store(true);
        }
    });
    ASSERT_NE(sub, 0u);
    ASSERT_TRUE(hx.service.hub().handle_inbound("still routed", hx.cfg.remote_control.token).ok());
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!delivered.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(delivered.load());
    hx.client.unsubscribe(first, sub);
    binder.execute_command(first, "off");
    hx.registry.destroy(first);
    hx.registry.destroy(second);
}

TEST(SessionChannelBinderIntegration,
     DuplicateCatalogCommandsAreCoalescedWhileOneIsRunning) {
    BinderHarness hx("session-command-coalesce");
    const auto session = hx.client.create_session({});
    std::mutex catalog_mu;
    std::condition_variable catalog_cv;
    bool entered = false;
    bool release = false;
    std::atomic<int> catalog_calls{0};
    auto deps = hx.binder_deps();
    deps.session_catalog = [&](const std::optional<std::string>&) {
        ++catalog_calls;
        std::unique_lock<std::mutex> lk(catalog_mu);
        entered = true;
        catalog_cv.notify_all();
        catalog_cv.wait(lk, [&] { return release; });
        return std::vector<acecode::rc::RcSessionTarget>{};
    };
    acecode::rc::SessionChannelBinder binder(std::move(deps));
    ASSERT_TRUE(binder.execute_command(session, "").ok);
    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);

    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/session", hx.cfg.remote_control.token).ok());
    {
        std::unique_lock<std::mutex> lk(catalog_mu);
        ASSERT_TRUE(catalog_cv.wait_for(lk, 3s, [&] { return entered; }));
    }
    for (int i = 0; i < 64; ++i) {
        ASSERT_TRUE(hx.service.hub().handle_inbound(
            i % 2 == 0 ? "/sessions all" : "/resume search build",
            hx.cfg.remote_control.token).ok());
    }
    {
        std::lock_guard<std::mutex> lk(catalog_mu);
        release = true;
    }
    catalog_cv.notify_all();
    const bool reported_processing = wait_for_outbound_text(
        sender, "Session navigation is already processing.");
    EXPECT_TRUE(reported_processing);
    ASSERT_TRUE(wait_for_outbound_text(sender, "Recent sessions:"));
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(catalog_calls.load(), 1);

    binder.execute_command(session, "off");
    hx.registry.destroy(session);
}

TEST(SessionChannelBinderIntegration,
     ControlQueueRejectsFloodBeyondFixedCapacity) {
    BinderHarness hx("session-command-queue-cap");
    const auto session = hx.client.create_session({});
    std::mutex catalog_mu;
    std::condition_variable catalog_cv;
    bool entered = false;
    bool release = false;
    auto deps = hx.binder_deps();
    deps.session_catalog = [&](const std::optional<std::string>&) {
        std::unique_lock<std::mutex> lk(catalog_mu);
        entered = true;
        catalog_cv.notify_all();
        catalog_cv.wait(lk, [&] { return release; });
        return std::vector<acecode::rc::RcSessionTarget>{};
    };
    acecode::rc::SessionChannelBinder binder(std::move(deps));
    ASSERT_TRUE(binder.execute_command(session, "").ok);
    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);

    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/session", hx.cfg.remote_control.token).ok());
    {
        std::unique_lock<std::mutex> lk(catalog_mu);
        ASSERT_TRUE(catalog_cv.wait_for(lk, 3s, [&] { return entered; }));
    }
    std::atomic<int> inbound_failures{0};
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 4; ++producer) {
        producers.emplace_back([&] {
            for (int i = 0; i < 16; ++i) {
                if (!hx.service.hub().handle_inbound(
                        "/sessions invalid", hx.cfg.remote_control.token).ok()) {
                    ++inbound_failures;
                }
            }
        });
    }
    for (auto& producer : producers) producer.join();
    EXPECT_EQ(inbound_failures.load(), 0);
    {
        std::lock_guard<std::mutex> lk(catalog_mu);
        release = true;
    }
    catalog_cv.notify_all();
    ASSERT_TRUE(wait_for_outbound_text(
        sender, "Session navigation is already processing."));
    ASSERT_TRUE(wait_for_outbound_text(sender, "Recent sessions:"));
    const auto usage_count = [&] {
        std::size_t count = 0;
        for (const auto& message : sender->sent()) {
            if (message.text.find("Usage: /sessions") != std::string::npos) {
                ++count;
            }
        }
        return count;
    };
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (usage_count() < 8u && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(usage_count(), 8u);

    binder.execute_command(session, "off");
    hx.registry.destroy(session);
}

TEST(SessionChannelBinderIntegration,
     QueuedSelectionFromUnboundGenerationIsSilentlyDropped) {
    BinderHarness hx("session-command-stale-off");
    const auto first = hx.client.create_session({});
    const auto target = hx.client.create_session({});
    std::mutex catalog_mu;
    std::condition_variable catalog_cv;
    bool entered = false;
    bool release = false;
    std::atomic<int> selected{0};
    auto deps = hx.binder_deps();
    deps.session_catalog = [&](const std::optional<std::string>&) {
        std::unique_lock<std::mutex> lk(catalog_mu);
        entered = true;
        catalog_cv.notify_all();
        catalog_cv.wait(lk, [&] { return release; });
        return std::vector<acecode::rc::RcSessionTarget>{
            {target, "workspace-target", "C:/target", "target", "", "target",
             "2026-08-05T10:00:00Z", false, true, 0}};
    };
    deps.on_session_selected = [&](const acecode::rc::RcSessionTarget&) {
        ++selected;
    };
    acecode::rc::SessionChannelBinder binder(std::move(deps));
    ASSERT_TRUE(binder.execute_command(first, "").ok);
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/sessions", hx.cfg.remote_control.token).ok());
    {
        std::unique_lock<std::mutex> lk(catalog_mu);
        ASSERT_TRUE(catalog_cv.wait_for(lk, 3s, [&] { return entered; }));
    }
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/sessions 1", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(binder.execute_command(first, "off").ok);
    {
        std::lock_guard<std::mutex> lk(catalog_mu);
        release = true;
    }
    catalog_cv.notify_all();
    const bool stale_activation =
        hx.runner_log->wait_for_activations(2, std::chrono::seconds(7));
    EXPECT_FALSE(stale_activation);
    EXPECT_EQ(selected.load(), 0);
    EXPECT_TRUE(binder.bound_session_id().empty());
    EXPECT_FALSE(hx.service.running());

    hx.registry.destroy(first);
    hx.registry.destroy(target);
}

TEST(SessionChannelBinderIntegration,
     QueuedSelectionFromReplacedGenerationCannotOverrideNewBinding) {
    BinderHarness hx("session-command-stale-rebind");
    const auto first = hx.client.create_session({});
    const auto replacement = hx.client.create_session({});
    const auto stale_target = hx.client.create_session({});
    std::mutex catalog_mu;
    std::condition_variable catalog_cv;
    bool entered = false;
    bool release = false;
    std::atomic<int> selected{0};
    auto deps = hx.binder_deps();
    deps.session_catalog = [&](const std::optional<std::string>&) {
        std::unique_lock<std::mutex> lk(catalog_mu);
        entered = true;
        catalog_cv.notify_all();
        catalog_cv.wait(lk, [&] { return release; });
        return std::vector<acecode::rc::RcSessionTarget>{
            {stale_target, "workspace-stale", "C:/stale", "stale", "", "stale",
             "2026-08-05T10:00:00Z", false, true, 0}};
    };
    deps.on_session_selected = [&](const acecode::rc::RcSessionTarget&) {
        ++selected;
    };
    acecode::rc::SessionChannelBinder binder(std::move(deps));
    ASSERT_TRUE(binder.execute_command(first, "").ok);
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/session", hx.cfg.remote_control.token).ok());
    {
        std::unique_lock<std::mutex> lk(catalog_mu);
        ASSERT_TRUE(catalog_cv.wait_for(lk, 3s, [&] { return entered; }));
    }
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/resume 1", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(binder.execute_command(replacement, "").ok);
    {
        std::lock_guard<std::mutex> lk(catalog_mu);
        release = true;
    }
    catalog_cv.notify_all();
    const bool stale_activation =
        hx.runner_log->wait_for_activations(3, std::chrono::seconds(7));
    EXPECT_FALSE(stale_activation);
    EXPECT_EQ(selected.load(), 0);
    EXPECT_EQ(binder.bound_session_id(), replacement);

    binder.execute_command(replacement, "off");
    hx.registry.destroy(first);
    hx.registry.destroy(replacement);
    hx.registry.destroy(stale_target);
}

TEST(SessionChannelBinderIntegration,
     AcceptedInboundBeforeOffIsDeliveredExactlyOnceToOldSession) {
    BinderHarness hx("accepted-inbound-off-fence");
    const auto old_session = hx.client.create_session({});
    const auto other_session = hx.client.create_session({});
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    ASSERT_TRUE(binder.execute_command(old_session, "").ok);

    const std::string text = "accepted before off fence";
    MatchingUserMessageCounter old_messages(text);
    MatchingUserMessageCounter other_messages(text);
    const auto old_sub = hx.client.subscribe(
        old_session, [&](const acecode::SessionEvent& event) {
            old_messages.observe(event);
        });
    const auto other_sub = hx.client.subscribe(
        other_session, [&](const acecode::SessionEvent& event) {
            other_messages.observe(event);
        });
    ASSERT_NE(old_sub, 0u);
    ASSERT_NE(other_sub, 0u);

    InboundAcceptBarrier accepted_barrier;
    InboundSuspendLatch suspend_latch;
    acecode::rc::RemoteControlHub::InboundFenceTestHooks hooks;
    hooks.after_accept_before_dispatch = [&] {
        accepted_barrier.enter_and_wait();
    };
    hooks.after_reject_before_wait = [&] { suspend_latch.signal(); };
    hx.service.hub().set_inbound_fence_test_hooks(std::move(hooks));
    ScopedInboundFenceHooks reset_hooks(hx.service.hub());

    acecode::rc::InboundResult inbound_result;
    std::thread inbound([&] {
        inbound_result = hx.service.hub().handle_inbound(
            text, hx.cfg.remote_control.token);
    });
    if (!accepted_barrier.wait_until_entered()) {
        accepted_barrier.release();
        inbound.join();
        FAIL() << "accepted inbound did not reach the pre-dispatch barrier";
    }

    auto stopped = std::async(std::launch::async, [&] {
        return binder.execute_command(old_session, "off");
    });
    const bool suspend_rejected_route = suspend_latch.wait();
    EXPECT_TRUE(suspend_rejected_route);
    EXPECT_EQ(stopped.wait_for(0ms), std::future_status::timeout);
    EXPECT_EQ(old_messages.count(), 0u);
    EXPECT_EQ(other_messages.count(), 0u);

    accepted_barrier.release();
    inbound.join();
    ASSERT_TRUE(inbound_result.ok()) << inbound_result.message;
    ASSERT_EQ(stopped.wait_for(7s), std::future_status::ready);
    ASSERT_TRUE(stopped.get().ok);
    ASSERT_TRUE(old_messages.wait_for(1));
    EXPECT_EQ(old_messages.count(), 1u);
    EXPECT_EQ(other_messages.count(), 0u);
    EXPECT_TRUE(binder.bound_session_id().empty());

    hx.client.unsubscribe(old_session, old_sub);
    hx.client.unsubscribe(other_session, other_sub);
    hx.registry.destroy(old_session);
    hx.registry.destroy(other_session);
}

TEST(SessionChannelBinderIntegration,
     AcceptedInboundBeforeRebindIsDeliveredExactlyOnceToOldSession) {
    BinderHarness hx("accepted-inbound-rebind-fence");
    const auto old_session = hx.client.create_session({});
    const auto target_session = hx.client.create_session({});
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    ASSERT_TRUE(binder.execute_command(old_session, "").ok);

    const std::string text = "accepted before rebind fence";
    MatchingUserMessageCounter old_messages(text);
    MatchingUserMessageCounter target_messages(text);
    const auto old_sub = hx.client.subscribe(
        old_session, [&](const acecode::SessionEvent& event) {
            old_messages.observe(event);
        });
    const auto target_sub = hx.client.subscribe(
        target_session, [&](const acecode::SessionEvent& event) {
            target_messages.observe(event);
        });
    ASSERT_NE(old_sub, 0u);
    ASSERT_NE(target_sub, 0u);

    InboundAcceptBarrier accepted_barrier;
    InboundSuspendLatch suspend_latch;
    acecode::rc::RemoteControlHub::InboundFenceTestHooks hooks;
    hooks.after_accept_before_dispatch = [&] {
        accepted_barrier.enter_and_wait();
    };
    hooks.after_reject_before_wait = [&] { suspend_latch.signal(); };
    hx.service.hub().set_inbound_fence_test_hooks(std::move(hooks));
    ScopedInboundFenceHooks reset_hooks(hx.service.hub());

    acecode::rc::InboundResult inbound_result;
    std::thread inbound([&] {
        inbound_result = hx.service.hub().handle_inbound(
            text, hx.cfg.remote_control.token);
    });
    if (!accepted_barrier.wait_until_entered()) {
        accepted_barrier.release();
        inbound.join();
        FAIL() << "accepted inbound did not reach the pre-dispatch barrier";
    }

    auto rebound = std::async(std::launch::async, [&] {
        return binder.execute_command(target_session, "");
    });
    const bool suspend_rejected_route = suspend_latch.wait();
    EXPECT_TRUE(suspend_rejected_route);
    EXPECT_EQ(rebound.wait_for(0ms), std::future_status::timeout);
    EXPECT_EQ(old_messages.count(), 0u);
    EXPECT_EQ(target_messages.count(), 0u);

    accepted_barrier.release();
    inbound.join();
    ASSERT_TRUE(inbound_result.ok()) << inbound_result.message;
    ASSERT_EQ(rebound.wait_for(7s), std::future_status::ready);
    ASSERT_TRUE(rebound.get().ok);
    ASSERT_TRUE(old_messages.wait_for(1));
    EXPECT_EQ(old_messages.count(), 1u);
    EXPECT_EQ(target_messages.count(), 0u);
    EXPECT_EQ(binder.bound_session_id(), target_session);

    hx.client.unsubscribe(old_session, old_sub);
    hx.client.unsubscribe(target_session, target_sub);
    binder.execute_command(target_session, "off");
    hx.registry.destroy(old_session);
    hx.registry.destroy(target_session);
}

TEST(SessionChannelBinderIntegration,
     ListOutputLeasePreventsRebindFromRetargetingQueuedText) {
    BinderHarness hx("session-output-list-rebind");
    const auto first = hx.client.create_session({});
    const auto second = hx.client.create_session({});
    ControlPublishBarrier barrier;
    auto deps = hx.binder_deps();
    deps.session_catalog = [&](const std::optional<std::string>&) {
        return std::vector<acecode::rc::RcSessionTarget>{
            {first, "workspace-first", "C:/first", "first", "", "first",
             "2026-08-05T10:00:00Z", false, true, 0}};
    };
    deps.before_control_publish =
        [&](const std::string& source, const std::string& text) {
            barrier.hook(source, text);
        };
    acecode::rc::SessionChannelBinder binder(std::move(deps));
    ASSERT_TRUE(binder.execute_command(first, "").ok);
    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);

    barrier.arm("Recent sessions:");
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/sessions", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(barrier.wait_until_entered());
    EXPECT_EQ(barrier.source_session(), first);

    auto rebound = std::async(std::launch::async, [&] {
        return binder.execute_command(second, "");
    });
    ASSERT_TRUE(hx.runner_log->wait_for_activations(2, 3s));
    EXPECT_EQ(rebound.wait_for(100ms), std::future_status::timeout);
    barrier.release();
    ASSERT_EQ(rebound.wait_for(5s), std::future_status::ready);
    ASSERT_TRUE(rebound.get().ok);
    ASSERT_TRUE(wait_for_outbound_text(sender, "Recent sessions:"));
    for (const auto& message : sender->sent()) {
        if (message.text.find("Recent sessions:") != std::string::npos) {
            EXPECT_EQ(message.session_id, first);
        }
    }

    binder.execute_command(second, "off");
    hx.registry.destroy(first);
    hx.registry.destroy(second);
}

TEST(SessionChannelBinderIntegration,
     ErrorOutputLeasePreventsOffFromStoppingItsOldRouteMidSend) {
    BinderHarness hx("session-output-error-off");
    const auto session = hx.client.create_session({});
    ControlPublishBarrier barrier;
    auto deps = hx.binder_deps();
    deps.session_catalog = [](const std::optional<std::string>&) {
        return std::vector<acecode::rc::RcSessionTarget>{};
    };
    deps.before_control_publish =
        [&](const std::string& source, const std::string& text) {
            barrier.hook(source, text);
        };
    acecode::rc::SessionChannelBinder binder(std::move(deps));
    ASSERT_TRUE(binder.execute_command(session, "").ok);
    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);

    barrier.arm("Invalid session number");
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/session 1", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(barrier.wait_until_entered());
    auto stopped = std::async(std::launch::async, [&] {
        return binder.execute_command(session, "off");
    });
    EXPECT_EQ(stopped.wait_for(100ms), std::future_status::timeout);
    barrier.release();
    ASSERT_EQ(stopped.wait_for(5s), std::future_status::ready);
    ASSERT_TRUE(stopped.get().ok);
    ASSERT_TRUE(wait_for_outbound_text(sender, "Invalid session number"));
    for (const auto& message : sender->sent()) {
        if (message.text.find("Invalid session number") != std::string::npos) {
            EXPECT_EQ(message.session_id, session);
        }
    }

    hx.registry.destroy(session);
}

TEST(SessionChannelBinderIntegration,
     SwitchSuccessOutputLeasePreventsThirdBindingFromClaimingIt) {
    BinderHarness hx("session-output-success-rebind");
    const auto first = hx.client.create_session({});
    const auto target = hx.client.create_session({});
    const auto third = hx.client.create_session({});
    ControlPublishBarrier barrier;
    auto sender = std::make_shared<CaptureSender>();
    std::atomic<int> selected_count{0};
    std::mutex selected_mu;
    std::string selected_session;
    auto deps = hx.binder_deps();
    deps.session_catalog = [&](const std::optional<std::string>&) {
        return std::vector<acecode::rc::RcSessionTarget>{
            {target, "workspace-target", "C:/target", "target", "", "target",
             "2026-08-05T10:00:00Z", false, true, 0}};
    };
    deps.before_control_publish =
        [&](const std::string& source, const std::string& text) {
            if (text.find("Remote connection switched to:") !=
                std::string::npos) {
                hx.service.hub().set_outbound_sender(sender);
            }
            barrier.hook(source, text);
        };
    deps.on_session_selected = [&](const acecode::rc::RcSessionTarget& selected) {
        {
            std::lock_guard<std::mutex> lk(selected_mu);
            selected_session = selected.session_id;
        }
        ++selected_count;
    };
    acecode::rc::SessionChannelBinder binder(std::move(deps));
    ASSERT_TRUE(binder.execute_command(first, "").ok);
    hx.service.hub().set_outbound_sender(sender);
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/sessions", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(wait_for_outbound_text(sender, "Recent sessions:"));

    barrier.arm("Remote connection switched to:");
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/resume 1", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(barrier.wait_until_entered());
    EXPECT_EQ(barrier.source_session(), target);
    EXPECT_EQ(selected_count.load(), 1);
    {
        std::lock_guard<std::mutex> lk(selected_mu);
        EXPECT_EQ(selected_session, target);
    }
    auto rebound = std::async(std::launch::async, [&] {
        return binder.execute_command(third, "");
    });
    ASSERT_TRUE(hx.runner_log->wait_for_activations(3, 3s));
    EXPECT_EQ(rebound.wait_for(100ms), std::future_status::timeout);
    barrier.release();
    ASSERT_EQ(rebound.wait_for(5s), std::future_status::ready);
    ASSERT_TRUE(rebound.get().ok);
    ASSERT_TRUE(wait_for_outbound_text(sender, "Remote connection switched to:"));
    for (const auto& message : sender->sent()) {
        if (message.text.find("Remote connection switched to:") !=
            std::string::npos) {
            EXPECT_EQ(message.session_id, target);
        }
    }
    EXPECT_EQ(binder.bound_session_id(), third);
    EXPECT_EQ(selected_count.load(), 1);

    binder.execute_command(third, "off");
    hx.registry.destroy(first);
    hx.registry.destroy(target);
    hx.registry.destroy(third);
}

TEST(SessionChannelBinderIntegration,
     SwitchPersistenceFailureKeepsOldRuntimeConfigAndSelectionCallback) {
    BinderHarness hx("session-switch-persist-failure");
    const auto first = hx.client.create_session({});
    const auto target = hx.client.create_session({});
    auto store = std::make_shared<ThrowingConfigStore>();
    store->disk = hx.cfg;
    std::atomic<int> selected{0};
    auto deps = hx.binder_deps();
    deps.load_disk_config = [store] { return store->load(); };
    deps.save_disk_config = [store](const acecode::AppConfig& config) {
        store->save(config);
    };
    deps.session_catalog = [&](const std::optional<std::string>&) {
        return std::vector<acecode::rc::RcSessionTarget>{
            {target, "workspace-target", "C:/target", "target", "", "target",
             "2026-08-05T10:00:00Z", false, true, 0}};
    };
    deps.on_session_selected = [&](const acecode::rc::RcSessionTarget&) {
        ++selected;
    };
    acecode::rc::SessionChannelBinder binder(std::move(deps));
    ASSERT_TRUE(binder.execute_command(first, "").ok);
    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/sessions", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(wait_for_outbound_text(sender, "Recent sessions:"));

    store->throw_save = true;
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "/sessions 1", hx.cfg.remote_control.token).ok());
    ASSERT_TRUE(wait_for_outbound_text(
        sender, "Failed to persist remote control binding"));
    EXPECT_EQ(binder.bound_session_id(), first);
    EXPECT_TRUE(hx.service.running());
    EXPECT_EQ(hx.cfg.remote_control.bound_session_id, first);
    EXPECT_EQ(store->disk.remote_control.bound_session_id, first);
    EXPECT_EQ(selected.load(), 0);
    ASSERT_TRUE(hx.runner_log->wait_for_activations(3, 3s));
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        EXPECT_EQ(hx.runner_log->activations.back()["session_id"], first);
    }

    std::mutex delivered_mu;
    std::condition_variable delivered_cv;
    bool delivered = false;
    const auto sub = hx.client.subscribe(first, [&](const acecode::SessionEvent& event) {
        if (event.kind == acecode::SessionEventKind::Message &&
            event.payload.value("role", std::string{}) == "user" &&
            event.payload.value("content", std::string{}) ==
                "still routed after failed switch") {
            std::lock_guard<std::mutex> lk(delivered_mu);
            delivered = true;
            delivered_cv.notify_all();
        }
    });
    ASSERT_NE(sub, 0u);
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "still routed after failed switch", hx.cfg.remote_control.token).ok());
    {
        std::unique_lock<std::mutex> lk(delivered_mu);
        EXPECT_TRUE(delivered_cv.wait_for(lk, 3s, [&] { return delivered; }));
    }
    hx.client.unsubscribe(first, sub);

    store->throw_save = false;
    binder.execute_command(first, "off");
    hx.registry.destroy(first);
    hx.registry.destroy(target);
}

TEST(SessionChannelBinderIntegration,
     OffPersistenceFailureKeepsLiveBindingAndOldConfig) {
    BinderHarness hx("session-off-persist-failure");
    const auto session = hx.client.create_session({});
    auto store = std::make_shared<ThrowingConfigStore>();
    store->disk = hx.cfg;
    auto deps = hx.binder_deps();
    deps.load_disk_config = [store] { return store->load(); };
    deps.save_disk_config = [store](const acecode::AppConfig& config) {
        store->save(config);
    };
    acecode::rc::SessionChannelBinder binder(std::move(deps));
    ASSERT_TRUE(binder.execute_command(session, "").ok);

    store->throw_load = true;
    const auto failed = binder.execute_command(session, "off");
    EXPECT_FALSE(failed.ok);
    EXPECT_NE(failed.message.find("Failed to persist remote control binding"),
              std::string::npos);
    EXPECT_EQ(binder.bound_session_id(), session);
    EXPECT_TRUE(hx.service.running());
    EXPECT_EQ(hx.cfg.remote_control.bound_session_id, session);
    EXPECT_EQ(store->disk.remote_control.bound_session_id, session);

    std::mutex delivered_mu;
    std::condition_variable delivered_cv;
    bool delivered = false;
    const auto sub = hx.client.subscribe(session, [&](const acecode::SessionEvent& event) {
        if (event.kind == acecode::SessionEventKind::Message &&
            event.payload.value("role", std::string{}) == "user" &&
            event.payload.value("content", std::string{}) ==
                "still routed after failed off") {
            std::lock_guard<std::mutex> lk(delivered_mu);
            delivered = true;
            delivered_cv.notify_all();
        }
    });
    ASSERT_NE(sub, 0u);
    ASSERT_TRUE(hx.service.hub().handle_inbound(
        "still routed after failed off", hx.cfg.remote_control.token).ok());
    {
        std::unique_lock<std::mutex> lk(delivered_mu);
        EXPECT_TRUE(delivered_cv.wait_for(lk, 3s, [&] { return delivered; }));
    }
    hx.client.unsubscribe(session, sub);

    store->throw_load = false;
    binder.execute_command(session, "off");
    hx.registry.destroy(session);
}
