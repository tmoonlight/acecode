#include "agent_loop.hpp"
#include "permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/session_trajectory.hpp"
#include "stub_provider.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

class AgentLoopTrajectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        cwd_ = fs::temp_directory_path() /
            fs::path("acecode-agent-trajectory-" +
                     std::to_string(std::chrono::steady_clock::now()
                         .time_since_epoch().count()));
        fs::create_directories(cwd_);
        project_dir_ = acecode::SessionStorage::get_project_dir(
            acecode::path_to_utf8(cwd_));
        std::error_code ec;
        fs::remove_all(acecode::path_from_utf8(project_dir_), ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(cwd_, ec);
        fs::remove_all(acecode::path_from_utf8(project_dir_), ec);
    }

    acecode::AgentCallbacks callbacks() {
        acecode::AgentCallbacks callbacks;
        callbacks.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lock(busy_mutex_);
            busy_ = busy;
            busy_cv_.notify_all();
        };
        callbacks.on_tool_confirm = [](
            const std::string&, const std::string&) {
            return acecode::PermissionResult::Allow;
        };
        return callbacks;
    }

    bool submit_and_wait(acecode::AgentLoop& loop,
                         std::chrono::milliseconds timeout = 5s) {
        {
            std::lock_guard<std::mutex> lock(busy_mutex_);
            busy_ = true;
        }
        loop.submit("inspect trajectory");
        std::unique_lock<std::mutex> lock(busy_mutex_);
        return busy_cv_.wait_for(lock, timeout, [this] { return !busy_; });
    }

    bool wait_until_busy(std::chrono::milliseconds timeout = 2s) {
        std::unique_lock<std::mutex> lock(busy_mutex_);
        return busy_cv_.wait_for(lock, timeout, [this] { return busy_; });
    }

    static std::vector<acecode::SessionTrajectoryRecord> records_of_type(
        const acecode::SessionTrajectoryPage& page,
        const std::string& type) {
        std::vector<acecode::SessionTrajectoryRecord> result;
        for (const auto& record : page.records) {
            if (record.type == type) result.push_back(record);
        }
        return result;
    }

    fs::path cwd_;
    std::string project_dir_;
    std::mutex busy_mutex_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
};

TEST_F(AgentLoopTrajectoryTest,
       RecordsRequestsReasoningFailedToolAndCompletedTurn) {
    acecode::SessionManager session;
    session.start_session(
        acecode::path_to_utf8(cwd_), "stub", "stub-1");

    acecode::ToolExecutor tools;
    acecode::ToolImpl tool;
    tool.definition.name = "trace_probe";
    tool.definition.description = "Trajectory schema probe";
    tool.definition.parameters = {
        {"type", "object"},
        {"properties", {{"target", {{"type", "string"}}}}},
        {"required", {"target"}},
    };
    tool.is_read_only = true;
    tool.execute = [](const std::string&, const acecode::ToolContext&) {
        std::string output;
        for (int line = 1; line <= 25; ++line) {
            output += "failure-line-" + std::to_string(line) + "\n";
        }
        return acecode::ToolResult{std::move(output), false};
    };
    tools.register_tool(std::move(tool));

    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    std::vector<acecode::StreamEvent> first_step;
    acecode::StreamEvent call_delta;
    call_delta.type = acecode::StreamEventType::ToolCallDelta;
    call_delta.tool_call = {"call-trace", "trace_probe", ""};
    first_step.push_back(call_delta);
    acecode::StreamEvent reasoning;
    reasoning.type = acecode::StreamEventType::ReasoningDelta;
    reasoning.content = "private reasoning";
    first_step.push_back(reasoning);
    acecode::StreamEvent text;
    text.type = acecode::StreamEventType::Delta;
    text.content = "calling probe";
    first_step.push_back(text);
    acecode::StreamEvent call;
    call.type = acecode::StreamEventType::ToolCall;
    call.tool_call = {"call-trace", "trace_probe", "{\"target\":\"alpha\"}"};
    first_step.push_back(call);
    acecode::StreamEvent usage;
    usage.type = acecode::StreamEventType::Usage;
    usage.usage.prompt_tokens = 40;
    usage.usage.completion_tokens = 8;
    usage.usage.total_tokens = 48;
    usage.usage.has_data = true;
    first_step.push_back(usage);
    acecode::StreamEvent done;
    done.type = acecode::StreamEventType::Done;
    done.finish_reason = "tool_calls";
    first_step.push_back(done);
    provider->push_events(std::move(first_step));
    provider->push_text("finished");

    acecode::PermissionManager permissions;
    auto accessor = [provider]() -> std::shared_ptr<acecode::LlmProvider> {
        return provider;
    };
    auto loop = std::make_unique<acecode::AgentLoop>(
        accessor, tools, callbacks(), acecode::path_to_utf8(cwd_), permissions);
    loop->set_session_manager(&session);
    ASSERT_TRUE(submit_and_wait(*loop));
    loop.reset();
    const std::string trajectory_path = session.current_trajectory_path();
    session.finalize();

    const auto page = acecode::SessionTrajectoryStorage::load_page(
        trajectory_path, 0, 1000);
    ASSERT_FALSE(page.records.empty());

    const auto requests = records_of_type(page, "model_request");
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].payload.value("step_index", 0), 1);
    EXPECT_EQ(requests[0].payload.value("provider", std::string{}), "stub");
    EXPECT_EQ(requests[0].payload.value("model", std::string{}), "stub-1");
    ASSERT_TRUE(requests[0].payload["messages"].is_array());
    ASSERT_TRUE(requests[0].payload["tools"].is_array());
    ASSERT_EQ(requests[0].payload["tools"].size(), 1u);
    EXPECT_EQ(requests[0].payload["tools"][0].value("name", std::string{}),
              "trace_probe");
    EXPECT_EQ(requests[0].payload["tools"][0].value(
                  "native_name", std::string{}),
              "trace_probe");
    EXPECT_EQ(requests[0].payload["tools"][0]["parameters"]["required"][0],
              "target");

    std::string request_dump = requests[0].payload.dump();
    std::transform(request_dump.begin(), request_dump.end(), request_dump.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    EXPECT_EQ(request_dump.find("authorization"), std::string::npos);
    EXPECT_EQ(request_dump.find("request_headers"), std::string::npos);
    EXPECT_EQ(request_dump.find("api_key"), std::string::npos);

    const auto first_outputs = records_of_type(page, "model_first_output");
    ASSERT_EQ(first_outputs.size(), 2u);
    EXPECT_EQ(first_outputs[0].payload.value("channel", std::string{}),
              "tool_call");

    const auto responses = records_of_type(page, "model_response");
    ASSERT_EQ(responses.size(), 2u);
    EXPECT_EQ(responses[0].payload.value("reasoning_content", std::string{}),
              "private reasoning");
    EXPECT_EQ(responses[0].payload.value("content", std::string{}),
              "calling probe");
    EXPECT_EQ(responses[0].payload["tool_calls"][0].value("id", std::string{}),
              "call-trace");
    EXPECT_EQ(responses[0].payload["usage"].value("total_tokens", 0), 48);

    const auto tool_ends = records_of_type(page, "tool_end");
    ASSERT_EQ(tool_ends.size(), 1u);
    EXPECT_FALSE(tool_ends[0].payload.value("success", true));
    EXPECT_NE(tool_ends[0].payload.value("output", std::string{}).find(
                  "failure-line-25"),
              std::string::npos);
    EXPECT_GE(tool_ends[0].payload.value("duration_ms", -1), 0);

    const auto turn_ends = records_of_type(page, "turn_end");
    ASSERT_EQ(turn_ends.size(), 1u);
    EXPECT_EQ(turn_ends[0].payload.value("outcome", std::string{}),
              "completed");
    EXPECT_GE(turn_ends[0].payload.value("duration_ms", -1), 0);
}

TEST_F(AgentLoopTrajectoryTest, AbortedStepAndTurnRemainClosed) {
    acecode::SessionManager session;
    session.start_session(
        acecode::path_to_utf8(cwd_), "stub", "stub-1");
    acecode::ToolExecutor tools;
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->set_latency_ms(400);
    provider->push_text("too late");
    acecode::PermissionManager permissions;
    auto accessor = [provider]() -> std::shared_ptr<acecode::LlmProvider> {
        return provider;
    };
    auto loop = std::make_unique<acecode::AgentLoop>(
        accessor, tools, callbacks(), acecode::path_to_utf8(cwd_), permissions);
    loop->set_session_manager(&session);

    {
        std::lock_guard<std::mutex> lock(busy_mutex_);
        busy_ = false;
    }
    loop->submit("abort trajectory");
    ASSERT_TRUE(wait_until_busy());
    const auto provider_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (provider->turn_count() == 0 &&
           std::chrono::steady_clock::now() < provider_deadline) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_GT(provider->turn_count(), 0);
    loop->abort();
    {
        std::unique_lock<std::mutex> lock(busy_mutex_);
        ASSERT_TRUE(busy_cv_.wait_for(lock, 5s, [this] { return !busy_; }));
    }
    loop.reset();
    const std::string trajectory_path = session.current_trajectory_path();
    session.finalize();

    const auto page = acecode::SessionTrajectoryStorage::load_page(
        trajectory_path, 0, 1000);
    const auto responses = records_of_type(page, "model_response");
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0].payload.value("status", std::string{}), "aborted");
    const auto turn_ends = records_of_type(page, "turn_end");
    ASSERT_EQ(turn_ends.size(), 1u);
    EXPECT_EQ(turn_ends[0].payload.value("outcome", std::string{}), "aborted");

    const auto finishes = records_of_type(page, "model_step_finish");
    ASSERT_EQ(finishes.size(), 1u);
    EXPECT_EQ(finishes[0].payload.value("reason", std::string{}), "aborted");
}

} // namespace
