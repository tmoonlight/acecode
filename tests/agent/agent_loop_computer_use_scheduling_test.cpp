#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "permissions/permissions.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/computer_use_tool.hpp"
#include "test_support/sandbox/test_support.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace acecode;
using namespace std::chrono_literals;

// Exercise the real Computer Use scheduling/approval metadata and AgentLoop.
// Only the OS boundary is replaced, so these tests never touch a real desktop.
class ComputerUseSchedulingHarness {
public:
    explicit ComputerUseSchedulingHarness(
        PermissionMode mode = PermissionMode::Default,
        PermissionResult answer = PermissionResult::Allow) {
        permissions_.set_mode(mode);
        for (auto tool : create_computer_use_tools()) {
            const auto name = tool.definition.name;
            tool.execute = [this, name](const std::string&, const ToolContext&) {
                std::lock_guard<std::mutex> lock(results_mu_);
                executions_.push_back(name);
                if (name == "computer_get_window_state") {
                    ++observations_;
                } else if (name == "computer_click" && (observations_ != 1 || released_)) {
                    return ToolResult{"The observation was superseded or released before the click", false};
                } else if (name == "computer_release") {
                    released_ = true;
                }
                return ToolResult{"ok", true};
            };
            tools_.register_tool(tool);
        }
        AgentCallbacks callbacks;
        callbacks.on_tool_confirm = [this, answer](const std::string& name, const std::string&) {
            std::lock_guard<std::mutex> lock(results_mu_);
            prompts_.push_back(name);
            return answer;
        };
        callbacks.on_tool_result = [this](const ChatMessage&, const std::string&, const ToolResult& result) {
            std::lock_guard<std::mutex> lock(results_mu_);
            results_.push_back(result);
        };
        callbacks.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lock(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        loop_ = std::make_unique<AgentLoop>(
            [this]() -> std::shared_ptr<LlmProvider> { return provider_; },
            tools_, callbacks, path_to_utf8(tree_.root), permissions_);
    }

    ~ComputerUseSchedulingHarness() { loop_.reset(); }

    bool run(std::vector<ToolCall> calls) {
        acecode_test::ScriptedResponse response;
        response.tool_calls = std::move(calls);
        provider_->push_response(std::move(response));
        provider_->push_text("done");
        {
            std::lock_guard<std::mutex> lock(busy_mu_);
            busy_ = true;
        }
        loop_->submit("Observe the app, perform the requested action, and release control.");
        std::unique_lock<std::mutex> lock(busy_mu_);
        const bool done = busy_cv_.wait_for(lock, 5s, [this] { return !busy_; });
        lock.unlock();
        loop_->shutdown();
        return done;
    }

    std::vector<std::string> executions() const {
        std::lock_guard<std::mutex> lock(results_mu_);
        return executions_;
    }
    std::vector<std::string> prompts() const {
        std::lock_guard<std::mutex> lock(results_mu_);
        return prompts_;
    }
    std::vector<ToolResult> results() const {
        std::lock_guard<std::mutex> lock(results_mu_);
        return results_;
    }
    std::vector<std::string> model_result_ids() const {
        std::vector<std::string> result;
        for (const auto& message : provider_->messages_for_turn(1)) {
            if (message.role == "tool") result.push_back(message.tool_call_id);
        }
        return result;
    }

private:
    sandbox::test::TempTree tree_;
    PermissionManager permissions_;
    ToolExecutor tools_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    std::unique_ptr<AgentLoop> loop_;
    std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
    mutable std::mutex results_mu_;
    std::vector<std::string> executions_;
    std::vector<std::string> prompts_;
    std::vector<ToolResult> results_;
    int observations_ = 0;
    bool released_ = false;
};
} // namespace

TEST(AgentLoopComputerUseScheduling, ObserveActReobserveReleasePreservesOrderAndOnlyWritePrompts) {
    ComputerUseSchedulingHarness harness;
    ASSERT_TRUE(harness.run({
        {"observe-before", "computer_get_window_state", R"({"window":101})"},
        {"click", "computer_click", R"({"window":101,"observation_id":"fixture","element_index":1})"},
        {"observe-after", "computer_get_window_state", R"({"window":101})"},
        {"release", "computer_release", "{}"},
    }));
    EXPECT_EQ(harness.executions(), (std::vector<std::string>{
        "computer_get_window_state", "computer_click", "computer_get_window_state", "computer_release"}));
    EXPECT_EQ(harness.prompts(), (std::vector<std::string>{"computer_click"}));
    const auto results = harness.results();
    ASSERT_EQ(results.size(), 4u);
    for (const auto& result : results) EXPECT_TRUE(result.success) << result.output;
    EXPECT_EQ(harness.model_result_ids(), (std::vector<std::string>{
        "observe-before", "click", "observe-after", "release"}));
}

TEST(AgentLoopComputerUseScheduling, PlanModeSerialObservationsAndReleaseDoNotPrompt) {
    ComputerUseSchedulingHarness harness(PermissionMode::Plan, PermissionResult::Deny);
    ASSERT_TRUE(harness.run({
        {"windows", "computer_list_windows", "{}"},
        {"observe", "computer_get_window_state", R"({"window":101})"},
        {"release", "computer_release", "{}"},
    }));
    EXPECT_TRUE(harness.prompts().empty());
    EXPECT_EQ(harness.executions(), (std::vector<std::string>{
        "computer_list_windows", "computer_get_window_state", "computer_release"}));
    const auto results = harness.results();
    ASSERT_EQ(results.size(), 3u);
    for (const auto& result : results) EXPECT_TRUE(result.success) << result.output;
}

TEST(AgentLoopComputerUseScheduling, DeniedActionDoesNotBlockFollowingObservationAndRelease) {
    ComputerUseSchedulingHarness harness(PermissionMode::Default, PermissionResult::Deny);
    ASSERT_TRUE(harness.run({
        {"observe-before", "computer_get_window_state", R"({"window":101})"},
        {"click", "computer_click", R"({"window":101,"observation_id":"fixture","element_index":1})"},
        {"observe-after", "computer_get_window_state", R"({"window":101})"},
        {"release", "computer_release", "{}"},
    }));
    EXPECT_EQ(harness.prompts(), (std::vector<std::string>{"computer_click"}));
    EXPECT_EQ(harness.executions(), (std::vector<std::string>{
        "computer_get_window_state", "computer_get_window_state", "computer_release"}));
    const auto results = harness.results();
    ASSERT_EQ(results.size(), 4u);
    EXPECT_TRUE(results[0].success);
    EXPECT_FALSE(results[1].success);
    EXPECT_TRUE(results[2].success);
    EXPECT_TRUE(results[3].success);
}
