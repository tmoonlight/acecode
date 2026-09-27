#pragma once

#include "agent_loop.hpp"
#include "agent_loop/stub_provider.hpp"
#include "headless/headless_mode.hpp"
#include "hooks/hook_manager.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/session_trajectory.hpp"
#include "tool/tool_protocol_names.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace acecode_test::characterization {
using namespace std::chrono_literals;
using Json = nlohmann::json;

// 场景:断言或构造中途失败。期望:进程环境仍恢复原值;否则后续用例会读错用户目录。
class ScopedEnvironment {
public:
    ScopedEnvironment(std::string name, const std::string& value)
        : name_(std::move(name)) {
        if (const char* old = std::getenv(name_.c_str())) previous_ = old;
        assign(value.c_str());
    }
    ~ScopedEnvironment() { assign(previous_ ? previous_->c_str() : nullptr); }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
private:
    void assign(const char* value) const {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value ? value : "");
#else
        if (value) setenv(name_.c_str(), value, 1);
        else unsetenv(name_.c_str());
#endif
    }
    std::string name_;
    std::optional<std::string> previous_;
};

class ScopedHeadless {
public:
    explicit ScopedHeadless(bool value) : previous_(acecode::headless::active()) {
        acecode::headless::set_active(value);
    }
    ~ScopedHeadless() { acecode::headless::set_active(previous_); }
    ScopedHeadless(const ScopedHeadless&) = delete;
    ScopedHeadless& operator=(const ScopedHeadless&) = delete;
private:
    bool previous_;
};

struct BorrowedProbeDirectory {
    std::filesystem::path path;
};

class TemporaryDirectory {
private:
    std::filesystem::path temporary_root_ = std::filesystem::canonical(std::filesystem::temp_directory_path());

public:
    TemporaryDirectory() : path(create_owned(temporary_root_)), owned_(true) {}
    explicit TemporaryDirectory(BorrowedProbeDirectory borrowed)
        : path(validate_borrowed(borrowed.path, temporary_root_)), owned_(false) {}
    ~TemporaryDirectory() {
        if (!owned_) return;
        try {
            // 仅删除本对象新建的唯一目录,并在删除前重验位置与身份;环境变量不授予所有权。
            if (validate_borrowed(path, temporary_root_) == path) {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } catch (const std::exception&) {
            // 目录已消失或身份被改动时保留现场,不扩大递归删除的目标。
        }
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const std::filesystem::path path;

private:
    static constexpr const char* marker_name = ".p0-11-probe-root";

    static std::filesystem::path create_owned(const std::filesystem::path& temporary_root) {
        static std::atomic<unsigned> next{0};
        for (int attempt = 0; attempt < 10; ++attempt) {
            const auto candidate = temporary_root / ("acecode-p0-11-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(next.fetch_add(1)));
            if (!std::filesystem::create_directory(candidate)) continue;
            std::ofstream marker(candidate / marker_name, std::ios::binary);
            marker << candidate.filename().string();
            marker.close();
            if (!marker) throw std::runtime_error("cannot mark P0-11 temporary directory");
            return validate_borrowed(candidate, temporary_root);
        }
        throw std::runtime_error("cannot create unique P0-11 temporary directory");
    }

    static std::filesystem::path validate_borrowed(const std::filesystem::path& selected,
                                                 const std::filesystem::path& temporary_root) {
        if (!selected.is_absolute() || std::filesystem::is_symlink(std::filesystem::symlink_status(selected))) {
            throw std::runtime_error("invalid P0-11 borrowed directory");
        }
        const auto canonical = std::filesystem::canonical(selected);
        const auto name = canonical.filename().string();
        if (canonical.parent_path() != temporary_root || name.rfind("acecode-p0-11-", 0) != 0) {
            throw std::runtime_error("P0-11 borrowed directory is outside its temporary root");
        }
        std::ifstream marker(canonical / marker_name, std::ios::binary);
        const std::string identity((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
        if (!marker || identity != name) throw std::runtime_error("P0-11 borrowed directory identity mismatch");
        return canonical;
    }
    bool owned_;
};

// 全部会话与规则文件只落本例临时 HOME,宿主/模型映射恢复后才删除临时目录。
class Isolation {
public:
    Isolation()
        : home_("HOME", acecode::path_to_utf8(directory.path)),
          profile_("USERPROFILE", acecode::path_to_utf8(directory.path)),
          mode_(acecode::override_run_mode_for_test(acecode::RunMode::User)) {}
    explicit Isolation(BorrowedProbeDirectory root)
        : directory(std::move(root)), home_("HOME", acecode::path_to_utf8(directory.path)),
          profile_("USERPROFILE", acecode::path_to_utf8(directory.path)),
          mode_(acecode::override_run_mode_for_test(acecode::RunMode::User)) {}
    ~Isolation() { acecode::override_run_mode_for_test(mode_); }
    TemporaryDirectory directory;
private:
    ScopedEnvironment home_;
    ScopedEnvironment profile_;
    ScopedHeadless headless_{false};
    acecode::ScopedModelToolNameMappings mappings_{acecode::ToolProtocolNameMappings{}};
    acecode::RunMode mode_;
};

// listener、工具闭包与主线程共享观测结果,寿命由这些参与方共同持有。
struct Observation {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<acecode::SessionEvent> events;
    std::vector<Json> hooks;
    std::vector<acecode::security::AuditEntry> audits;
    std::vector<Json> displays;
    std::vector<Json> executions;
    std::vector<acecode::ToolResult> results;
    std::vector<std::string> decisions;
    std::vector<bool> busy_callbacks;
    int done = 0;
    int turns_finished = 0;
    int confirmations = 0;
    acecode::PermissionResult answer = acecode::PermissionResult::Allow;
    std::function<void()> after_turn;

    void decision(std::string entry) {
        std::lock_guard<std::mutex> lock(mutex);
        decisions.push_back(std::move(entry));
    }
};

inline acecode::HookProcessResult hook_output(const Json& output) {
    acecode::HookProcessResult result;
    result.started = true;
    result.exit_code = 0;
    result.stdout_text = output.dump();
    result.output = result.stdout_text;
    return result;
}

inline acecode::NormalizedHook hook_for(const std::string& event) {
    acecode::NormalizedHook hook;
    hook.id = "characterize-" + event;
    hook.source_id = "p0-11";
    hook.event_name = event;
    hook.matcher = "*";
    hook.kind = acecode::HookHandlerKind::Command;
    hook.command.command = "injected-hook";
    hook.command.timeout_seconds = 2;
    hook.trust_status = acecode::HookTrustStatus::Trusted;
    return hook;
}

class Harness {
public:
    explicit Harness(Isolation& isolation, std::string name = "case",
                     std::shared_ptr<acecode::LlmProvider> custom = {},
                     bool confirmation = true)
        : cwd(isolation.directory.path / name),
          provider(std::make_shared<StubLlmProvider>()),
          active_provider(custom ? std::move(custom) : provider),
          observed(std::make_shared<Observation>()) {
        std::filesystem::create_directories(cwd);
        session->start_session(acecode::path_to_utf8(cwd), "stub", "stub-1", name);
        acecode::AgentCallbacks callbacks;
        callbacks.on_message = [state = observed](const std::string& role,
                                                  const std::string& content, bool) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->displays.push_back({{"role", role}, {"content", content}});
        };
        callbacks.on_tool_result = [state = observed](const acecode::ChatMessage&,
            const std::string&, const acecode::ToolResult& result) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->results.push_back(result);
        };
        callbacks.on_busy_changed = [state = observed](bool busy) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->busy_callbacks.push_back(busy);
        };
        callbacks.on_turn_finished = [state = observed](const std::string&) {
            std::function<void()> after_turn;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                ++state->turns_finished;
                after_turn = state->after_turn;
            }
            if (after_turn) after_turn();
        };
        if (confirmation) {
            callbacks.on_tool_confirm = [state = observed](const std::string&,
                                                          const std::string&) {
                std::lock_guard<std::mutex> lock(state->mutex);
                ++state->confirmations;
                state->decisions.push_back("confirm");
                return state->answer;
            };
        }
        loop = std::make_unique<acecode::AgentLoop>(
            [snapshot = active_provider] { return snapshot; }, tools, callbacks,
            acecode::path_to_utf8(cwd), permissions);
        loop->set_session_manager(session.get());
        loop->set_exec_rules({});
        loop->set_exec_rules_dir_for_tests(acecode::path_to_utf8(cwd / "rules"));
        loop->set_sandbox_availability_for_tests(true);
        loop->set_audit_sink([state = observed](const acecode::security::AuditEntry& entry) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->audits.push_back(entry);
            state->decisions.push_back("audit:" + entry.decision + ":" + entry.source);
        });
        subscription_ = loop->events().subscribe([state = observed](const acecode::SessionEvent& event) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->events.push_back(event);
            if (event.kind == acecode::SessionEventKind::Done) ++state->done;
            state->changed.notify_all();
        });
    }

    ~Harness() {
        // worker 停止后才退订及释放 hooks/session,失败退出也不留下悬垂 listener。
        if (loop) {
            loop->shutdown();
            loop->events().unsubscribe(subscription_);
            loop.reset();
        }
        hooks.reset();
        session->finalize();
    }

    void install_hooks(std::vector<std::string> events,
                       std::function<Json(const Json&)> reply = {}) {
        acecode::HookRegistrySnapshot registry;
        registry.feature_enabled = true;
        for (const auto& event : events) registry.hooks.push_back(hook_for(event));
        hooks = std::make_unique<acecode::HookManager>(std::move(registry),
            acecode::HookProcessRunner{},
            [state = observed, reply = std::move(reply)](
                const std::string&, const std::string& input, int, const std::string&) {
                const auto payload = Json::parse(input);
                const auto event = payload.value("hook_event_name", std::string{});
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->hooks.push_back(payload);
                    if (event == "PermissionRequest" || event == "PermissionResolved") {
                        state->decisions.push_back(event == "PermissionRequest"
                            ? event : event + ":" + payload.value("permission_decision", "") +
                              ":" + payload.value("permission_source", ""));
                    }
                }
                return hook_output(reply ? reply(payload) : Json::object());
            });
        loop->set_hook_manager(hooks.get());
    }

    acecode::ToolImpl probe(std::string name, bool read_only, std::string output = "probe ok") {
        acecode::ToolImpl tool;
        tool.definition.name = std::move(name);
        tool.definition.description = "P0-11 deterministic boundary probe";
        tool.definition.parameters = {{"type", "object"}, {"properties", Json::object()}};
        tool.is_read_only = read_only;
        tool.execute = [state = observed, name = tool.definition.name, output = std::move(output)](
            const std::string& arguments, const acecode::ToolContext& context) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->executions.push_back({{"tool", name}, {"arguments", arguments},
                {"sandbox", context.exec_sandbox
                    ? acecode::sandbox::sandbox_mode_name(context.exec_sandbox->policy.mode)
                    : "full-access"}});
            state->decisions.push_back("execute");
            return acecode::ToolResult{output, true};
        };
        return tool;
    }

    bool perform(const std::function<void()>& action) {
        int expected;
        {
            std::lock_guard<std::mutex> lock(observed->mutex);
            expected = observed->done + 1;
        }
        action();
        {
            std::unique_lock<std::mutex> lock(observed->mutex);
            if (!observed->changed.wait_for(lock, 5s,
                    [state = observed, expected] { return state->done >= expected; })) return false;
        }
        // Done 在 worker 函数返回之前发布;队列 fence 保证下一条断言看到 RAII 收尾。
        auto fence = std::make_shared<std::promise<void>>();
        auto ready = fence->get_future();
        loop->enqueue_control([fence] { fence->set_value(); return true; });
        return ready.wait_for(5s) == std::future_status::ready;
    }

    bool run_calls(std::vector<acecode::ToolCall> calls) {
        ScriptedResponse response;
        response.tool_calls = std::move(calls);
        provider->push_response(std::move(response));
        provider->push_text("finished");
        return perform([loop = loop.get()] { loop->submit("characterize this turn"); });
    }

    std::vector<acecode::SessionTrajectoryRecord> trajectory(const std::string& type) {
        const auto page = acecode::SessionTrajectoryStorage::load_page(
            session->current_trajectory_path(), 0, 1000);
        std::vector<acecode::SessionTrajectoryRecord> found;
        for (const auto& record : page.records) if (record.type == type) found.push_back(record);
        return found;
    }

    std::filesystem::path cwd;
    // provider 与入口函数共享同一脚本实例,观测记录同时由 worker/listener/test 持有。
    std::shared_ptr<StubLlmProvider> provider;
    std::shared_ptr<acecode::LlmProvider> active_provider;
    std::shared_ptr<Observation> observed;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    // goal 用例的收尾回调临时 lock 租约,与 fixture 共同保证数据库仍存活。
    std::shared_ptr<acecode::SessionManager> session = std::make_shared<acecode::SessionManager>();
    std::unique_ptr<acecode::HookManager> hooks;
    std::unique_ptr<acecode::AgentLoop> loop;
private:
    acecode::EventDispatcher::SubscriptionId subscription_ = 0;
};

inline std::vector<acecode::SessionEvent> events_of(
    const Observation& observed, acecode::SessionEventKind kind) {
    std::vector<acecode::SessionEvent> found;
    for (const auto& event : observed.events) if (event.kind == kind) found.push_back(event);
    return found;
}
} // namespace acecode_test::characterization
