#include <gtest/gtest.h>

#include "session_host/follow_up_prompt.hpp"
#include "session/session_serializer.hpp"
#include "session/turn_timing.hpp"

#include <filesystem>
#include <fstream>
#include <random>

using namespace acecode;
namespace {
class FollowUpPromptTest : public testing::Test {
protected:
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("acecode-follow-up-" + std::to_string(std::random_device{}()) + ".jsonl");
    void TearDown() override { std::filesystem::remove(path); }
    void append(const ChatMessage& message) {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << serialize_message(message) << '\n';
    }
    void turn(std::string id = "u1", std::string outcome = "completed", std::int64_t completed = 200) {
        ChatMessage user;
        user.role = "user"; user.uuid = id; user.content = "Implement the parser";
        append(user);
        ChatMessage assistant;
        assistant.role = "assistant"; assistant.content = "The parser is implemented; tests remain.";
        assistant.reasoning_content = "PRIVATE_REASONING";
        assistant.tool_calls = {{{"function", {{"name", "SECRET_TOOL"}, {"arguments", "PRIVATE_ARGS"}}}}};
        append(assistant);
        append(make_turn_timing_message({id, 100, completed, completed - 100, outcome}, ""));
    }
};

class PromptProvider final : public LlmProvider {
public:
    ChatResponse response;
    std::vector<ChatMessage> received;
    std::vector<ToolDef> received_tools;
    bool allowed = true;
    bool supports_tool_free_chat() const override { return allowed; }
    ChatResponse chat(const std::vector<ChatMessage>& messages, const std::vector<ToolDef>& tools) override {
        received = messages; received_tools = tools; return response;
    }
    void chat_stream(const std::vector<ChatMessage>&, const std::vector<ToolDef>&,
                     const StreamCallback&, std::atomic<bool>* = nullptr) override {}
    std::string name() const override { return "follow-up-test"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "follow-up-test"; }
    void set_model(const std::string&) override {}
};
}

TEST_F(FollowUpPromptTest, LoadsStableCompletionWithoutHiddenContext) {
    turn();
    const auto context = read_follow_up_context(path.string());
    ASSERT_TRUE(context);
    EXPECT_EQ(context->completion_key, "u1:200");
    ASSERT_EQ(context->messages.size(), 2u);
    const auto messages = follow_up_messages(*context, "en-US");
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_NE(messages[0].content.find("Write in English"), std::string::npos);
    EXPECT_EQ(messages[1].content.find("PRIVATE"), std::string::npos);
    EXPECT_EQ(messages[1].content.find("SECRET_TOOL"), std::string::npos);
}

TEST_F(FollowUpPromptTest, PendingUserSupersedesCompletedTurn) {
    turn();
    ChatMessage user;
    user.role = "user"; user.uuid = "u2"; user.content = "Continue";
    append(user);
    EXPECT_FALSE(read_follow_up_context(path.string()));
}

TEST_F(FollowUpPromptTest, AbortedAndErroredTurnsAreIneligible) {
    turn("u1", "aborted");
    EXPECT_FALSE(read_follow_up_context(path.string()));
    turn("u2", "error", 300);
    EXPECT_FALSE(read_follow_up_context(path.string()));
    turn("u3", "completed", 400);
    ASSERT_TRUE(read_follow_up_context(path.string()));
    EXPECT_EQ(read_follow_up_context(path.string())->completion_key, "u3:400");
}

TEST_F(FollowUpPromptTest, RejectsTruncatedAndMalformedTail) {
    turn();
    { std::ofstream file(path, std::ios::app); file << "not json\n"; }
    EXPECT_FALSE(read_follow_up_context(path.string()));
    turn("u2", "completed", 300);
    { std::ofstream file(path, std::ios::app); file << "{\"role\":\"user\""; }
    EXPECT_FALSE(read_follow_up_context(path.string()));
}

TEST_F(FollowUpPromptTest, BoundsLargeHistoryAndVisibleMessageLengths) {
    { std::ofstream file(path, std::ios::binary); file << std::string(3 * 1024 * 1024, 'x') << '\n'; }
    for (int i = 0; i < 15; ++i) turn("u" + std::to_string(i), "completed", 200 + i);
    const auto context = read_follow_up_context(path.string());
    ASSERT_TRUE(context);
    EXPECT_EQ(context->completion_key, "u14:214");
    EXPECT_EQ(context->messages.size(), 12u);
    ChatMessage long_message;
    long_message.role = "assistant"; long_message.content = std::string(10000, 'a');
    append(long_message);
    EXPECT_LE(read_follow_up_context(path.string())->messages.back()["text"].get<std::string>().size(), 2400u);
}

TEST_F(FollowUpPromptTest, SupportsToolOnlyCompletionAndNullMetadata) {
    ChatMessage user;
    user.role = "user"; user.uuid = "u1"; user.content = "Do the task";
    append(user);
    ChatMessage tool;
    tool.role = "tool"; tool.content = "PRIVATE_FILE_CONTENT";
    tool.metadata = {{"tool_summary", {{"action", "Completed"}, {"object", "implementation"}}}};
    append(tool);
    append(make_turn_timing_message({"u1", 100, 200, 100, "completed"}, ""));
    const auto context = read_follow_up_context(path.string());
    ASSERT_TRUE(context);
    EXPECT_EQ(context->messages.size(), 2u);
    EXPECT_EQ(context->messages.dump().find("PRIVATE_FILE_CONTENT"), std::string::npos);
}

TEST(FollowUpPrompt, StrictResponseValidation) {
    EXPECT_EQ(parse_follow_up_prompt(R"({"prompt":"Run the tests."})"), "Run the tests.");
    EXPECT_EQ(parse_follow_up_prompt(R"({"prompt":""})"), "");
    EXPECT_FALSE(parse_follow_up_prompt("Run the tests."));
    EXPECT_FALSE(parse_follow_up_prompt(R"({"prompt":["Run tests"]})"));
    EXPECT_FALSE(parse_follow_up_prompt(R"({"prompt":"First\nSecond"})"));
    EXPECT_FALSE(parse_follow_up_prompt(nlohmann::json{{"prompt", std::string(641, 'x')}}.dump()));
}

TEST_F(FollowUpPromptTest, GenerationUsesToolFreeRequestAndRejectsToolCalls) {
    turn();
    PromptProvider provider;
    provider.response.content = R"({"prompt":"Run the tests."})";
    const auto context = *read_follow_up_context(path.string());
    EXPECT_EQ(generate_follow_up_prompt(provider, context, "en-US"), "Run the tests.");
    EXPECT_TRUE(provider.received_tools.empty());
    ASSERT_EQ(provider.received.size(), 2u);
    provider.response.tool_calls.push_back({"call1", "shell", "{}"});
    EXPECT_FALSE(generate_follow_up_prompt(provider, context, "en-US"));
}

TEST_F(FollowUpPromptTest, CancellationAndNativeRuntimeNeverStartRequest) {
    turn();
    PromptProvider provider;
    const auto context = *read_follow_up_context(path.string());
    std::atomic<bool> cancelled{true};
    EXPECT_FALSE(generate_follow_up_prompt(provider, context, "en-US", &cancelled));
    EXPECT_TRUE(provider.received.empty());
    provider.allowed = false;
    EXPECT_FALSE(generate_follow_up_prompt(provider, context, "en-US"));
    EXPECT_TRUE(provider.received.empty());
}
