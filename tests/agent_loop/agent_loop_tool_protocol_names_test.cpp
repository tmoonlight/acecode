#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "provider/dsml_tool_call_recovery.hpp"
#include "stub_provider.hpp"
#include "tool/tool_executor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path make_protocol_temp_dir() {
    static std::atomic<unsigned int> sequence{0};
    auto path = fs::temp_directory_path() /
        ("acecode_tool_protocol_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
         std::to_string(sequence.fetch_add(1)));
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}

class DsmlRecoveryProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>&,
        const std::vector<acecode::ToolDef>&) override {
        return {};
    }

    void chat_stream(
        const std::vector<acecode::ChatMessage>&,
        const std::vector<acecode::ToolDef>& tools,
        const acecode::StreamCallback& callback,
        std::atomic<bool>* = nullptr) override {
        const int turn = turns_.fetch_add(1);
        if (turn == 0) {
            const std::string raw =
                u8"I will write.\n<｜DSML｜tool_calls>"
                u8"<｜DSML｜invoke name=\"write\">"
                u8"<｜DSML｜parameter name=\"value\" string=\"true\">"
                u8"from-dsml</｜DSML｜parameter>"
                u8"</｜DSML｜invoke></｜DSML｜tool_calls>";
            auto recovered = acecode::recover_dsml_tool_calls(raw, tools);
            if (!recovered.recovered) {
                acecode::StreamEvent error;
                error.type = acecode::StreamEventType::Error;
                error.error = recovered.error;
                callback(error);
                return;
            }
            if (!recovered.visible_text.empty()) {
                acecode::StreamEvent delta;
                delta.type = acecode::StreamEventType::Delta;
                delta.content = recovered.visible_text;
                callback(delta);
            }
            for (std::size_t i = 0; i < recovered.tool_calls.size(); ++i) {
                acecode::StreamEvent call;
                call.type = acecode::StreamEventType::ToolCall;
                call.tool_call = recovered.tool_calls[i];
                call.tool_index = static_cast<int>(i);
                callback(call);
            }
            acecode::StreamEvent done;
            done.type = acecode::StreamEventType::Done;
            done.finish_reason = "tool_calls";
            callback(done);
            return;
        }

        acecode::StreamEvent delta;
        delta.type = acecode::StreamEventType::Delta;
        delta.content = "done";
        callback(delta);
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        done.finish_reason = "stop";
        callback(done);
    }

    std::string name() const override { return "dsml-test"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return model_; }
    void set_model(const std::string& model) override { model_ = model; }
    int turns() const { return turns_.load(); }

private:
    std::atomic<int> turns_{0};
    std::string model_ = "dsml-test-model";
};

class ToolProtocolAgentHarness {
public:
    explicit ToolProtocolAgentHarness(
        std::string cwd,
        std::shared_ptr<acecode::LlmProvider> provider = {},
        bool tool_is_read_only = true)
        : cwd_(std::move(cwd)), provider_(std::move(provider)) {
        if (!provider_) {
            stub_provider_ = std::make_shared<acecode_test::StubLlmProvider>();
            provider_ = stub_provider_;
        }
        acecode::ToolImpl tool;
        tool.definition.name = "file_write";
        tool.definition.description = "shared boundary probe";
        tool.definition.parameters = {
            {"type", "object"},
            {"properties", {{"value", {{"type", "string"}}}}},
        };
        tool.is_read_only = tool_is_read_only;
        tool.execute = [this](const std::string& arguments,
                              const acecode::ToolContext&) {
            captured_arguments_ = arguments;
            calls_.fetch_add(1);
            return acecode::ToolResult{"write completed", true};
        };
        EXPECT_TRUE(tools_.register_tool(tool));

        acecode::AgentCallbacks callbacks;
        callbacks.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lock(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        callbacks.on_tool_confirm = [this](const std::string&, const std::string&) {
            confirmations_.fetch_add(1);
            return acecode::PermissionResult::Allow;
        };
        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> {
            return provider_;
        };
        loop_ = std::make_unique<acecode::AgentLoop>(
            accessor, tools_, callbacks, cwd_, permissions_);
    }

    ~ToolProtocolAgentHarness() {
        if (loop_) loop_->shutdown();
    }

    acecode_test::StubLlmProvider& provider() { return *stub_provider_; }
    acecode::AgentLoop& loop() { return *loop_; }
    int calls() const { return calls_.load(); }
    int confirmations() const { return confirmations_.load(); }
    const std::string& captured_arguments() const { return captured_arguments_; }

    bool submit_and_wait(std::chrono::milliseconds timeout = 5s) {
        {
            std::lock_guard<std::mutex> lock(busy_mu_);
            busy_ = true;
        }
        loop_->submit("use the write tool");
        std::unique_lock<std::mutex> lock(busy_mu_);
        return busy_cv_.wait_for(lock, timeout, [this] { return !busy_; });
    }

private:
    std::string cwd_;
    std::shared_ptr<acecode::LlmProvider> provider_;
    std::shared_ptr<acecode_test::StubLlmProvider> stub_provider_;
    acecode::ToolExecutor tools_;
    acecode::PermissionManager permissions_;
    std::unique_ptr<acecode::AgentLoop> loop_;
    std::atomic<int> calls_{0};
    std::atomic<int> confirmations_{0};
    std::string captured_arguments_;
    std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
};

const acecode::ChatMessage* find_assistant_call(
    const std::vector<acecode::ChatMessage>& messages) {
    const auto it = std::find_if(
        messages.begin(), messages.end(), [](const acecode::ChatMessage& message) {
            return message.role == "assistant" && message.tool_calls.is_array() &&
                   !message.tool_calls.empty();
        });
    return it == messages.end() ? nullptr : &*it;
}

const acecode::ChatMessage* find_tool_result(
    const std::vector<acecode::ChatMessage>& messages,
    const std::string& id) {
    const auto it = std::find_if(
        messages.begin(), messages.end(), [&](const acecode::ChatMessage& message) {
            return message.role == "tool" && message.tool_call_id == id;
        });
    return it == messages.end() ? nullptr : &*it;
}

} // namespace

TEST(AgentLoopToolProtocolNames,
     TranslatesOutboundAndInboundAtSharedBoundaryWithCorrelation) {
    const fs::path cwd = make_protocol_temp_dir();
    ToolProtocolAgentHarness harness(cwd.string());
    const std::string arguments = R"({"value":"from-model"})";
    harness.provider().push_tool_call("write", arguments, "call-public-write");
    harness.provider().push_text("done");

    ASSERT_TRUE(harness.submit_and_wait());
    harness.loop().shutdown();

    EXPECT_EQ(harness.calls(), 1);
    EXPECT_EQ(harness.captured_arguments(), arguments);
    ASSERT_GE(harness.provider().turn_count(), 2);

    const auto first_tools = harness.provider().tools_for_turn(0);
    ASSERT_EQ(first_tools.size(), 1u);
    EXPECT_EQ(first_tools.front().name, "write");
    EXPECT_EQ(first_tools.front().description, "shared boundary probe");
    EXPECT_EQ(first_tools.front().parameters["properties"]["value"]["type"],
              "string");

    const auto first_messages = harness.provider().messages_for_turn(0);
    ASSERT_FALSE(first_messages.empty());
    EXPECT_EQ(first_messages.front().content.find("file_write"),
              std::string::npos);

    const auto followup_messages = harness.provider().messages_for_turn(1);
    const auto* provider_call = find_assistant_call(followup_messages);
    ASSERT_NE(provider_call, nullptr);
    ASSERT_EQ(provider_call->tool_calls.size(), 1u);
    EXPECT_EQ((*provider_call).tool_calls[0]["function"]["name"], "write");
    EXPECT_EQ((*provider_call).tool_calls[0]["function"]["arguments"],
              arguments);
    EXPECT_EQ((*provider_call).tool_calls[0]["id"], "call-public-write");
    const auto* provider_result =
        find_tool_result(followup_messages, "call-public-write");
    ASSERT_NE(provider_result, nullptr);
    EXPECT_EQ(provider_result->content, "write completed");

    const auto& internal_messages = harness.loop().messages();
    const auto* internal_call = find_assistant_call(internal_messages);
    ASSERT_NE(internal_call, nullptr);
    EXPECT_EQ(internal_call->tool_calls[0]["function"]["name"], "file_write");
    EXPECT_EQ(internal_call->tool_calls[0]["function"]["arguments"], arguments);
    EXPECT_EQ(internal_call->tool_calls[0]["id"], "call-public-write");
    ASSERT_NE(find_tool_result(internal_messages, "call-public-write"), nullptr);

    fs::remove_all(cwd);
}

TEST(AgentLoopToolProtocolNames,
     RecoveredDsmlUsesAliasPermissionAndExecutesExactlyOnce) {
    const fs::path cwd = make_protocol_temp_dir();
    auto provider = std::make_shared<DsmlRecoveryProvider>();
    ToolProtocolAgentHarness harness(cwd.string(), provider,
                                     /*tool_is_read_only=*/false);

    ASSERT_TRUE(harness.submit_and_wait());
    harness.loop().shutdown();

    EXPECT_EQ(provider->turns(), 2);
    EXPECT_EQ(harness.calls(), 1);
    EXPECT_EQ(harness.confirmations(), 1);
    EXPECT_EQ(nlohmann::json::parse(harness.captured_arguments())["value"],
              "from-dsml");

    const auto& messages = harness.loop().messages();
    const auto* internal_call = find_assistant_call(messages);
    ASSERT_NE(internal_call, nullptr);
    ASSERT_EQ(internal_call->tool_calls.size(), 1u);
    EXPECT_EQ(internal_call->tool_calls[0]["function"]["name"], "file_write");
    EXPECT_EQ(internal_call->content.find(u8"<｜DSML｜"), std::string::npos);
    const std::string call_id = internal_call->tool_calls[0]["id"];
    ASSERT_NE(find_tool_result(messages, call_id), nullptr);

    fs::remove_all(cwd);
}
