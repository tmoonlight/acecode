#include "test_support/agent_loop/characterization_fixture.hpp"
#include "commands/compact_prompt.hpp"

#include <algorithm>

namespace {
using namespace acecode;
using namespace acecode_test::characterization;

class CompactProbe : public acecode_test::StubLlmProvider {
public:
    ChatResponse chat(const std::vector<ChatMessage>& messages,
                      const std::vector<ToolDef>& tools) override {
        std::lock_guard<std::mutex> lock(mutex);
        compact_requests.push_back(messages);
        compact_tool_counts.push_back(tools.size());
        ChatResponse response;
        response.content = "Golden compact summary.";
        response.finish_reason = "stop";
        return response;
    }
    std::mutex mutex;
    std::vector<std::vector<ChatMessage>> compact_requests;
    std::vector<std::size_t> compact_tool_counts;
};

ChatMessage message(std::string role, std::string content) {
    ChatMessage result;
    result.role = std::move(role);
    result.content = std::move(content);
    return result;
}

struct PromptCase { const char* name; const char* model; bool automatic; };
class AgentLoopStaticPromptGolden : public testing::TestWithParam<PromptCase> {};

// 场景:模型族不同,压缩由首轮自动触发或手工触发。期望:压缩初始上下文和主请求的
// 静态 system 消息逐字节一致;回归会打穿缓存前缀或让摘要看到另一套系统约束。
TEST_P(AgentLoopStaticPromptGolden, CompactionInitialSystemEqualsMainRequestByteForByte) {
    Isolation isolation;
    auto provider = std::make_shared<CompactProbe>(); // 入口和测试共享请求记录器。
    provider->set_model(GetParam().model);
    Harness h(isolation, GetParam().name, provider);
    h.tools.register_tool(h.probe("file_read", true));
    h.loop->push_message(message("user", std::string(1800, 'U')));
    h.loop->push_message(message("assistant", std::string(1800, 'A')));
    h.loop->set_context_window(GetParam().automatic ? 100 : 1000000);
    provider->push_text("main response");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("new user input"); }));
    if (!GetParam().automatic) ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit_compact(); }));
    const auto main = provider->messages_for_turn(0);
    ASSERT_FALSE(main.empty());
    ASSERT_EQ(main.front().role, "system");
    std::lock_guard<std::mutex> lock(provider->mutex);
    ASSERT_EQ(provider->compact_requests.size(), 1u);
    ASSERT_FALSE(provider->compact_requests[0].empty());
    const auto& compact = provider->compact_requests[0];
    EXPECT_EQ(compact.front().role, "system");
    EXPECT_EQ(compact.front().content.size(), main.front().content.size());
    EXPECT_EQ(compact.front().content, main.front().content);
    EXPECT_EQ(provider->compact_tool_counts, (std::vector<std::size_t>{0}));
    EXPECT_EQ(compact.back().content, get_compact_prompt());
    if (GetParam().automatic) {
        for (const auto& item : compact) EXPECT_NE(item.content, "new user input");
    }
}

INSTANTIATE_TEST_SUITE_P(P0_11, AgentLoopStaticPromptGolden,
    testing::Values(PromptCase{"GptAutomatic", "gpt-5", true},
                    PromptCase{"GptManual", "gpt-5", false},
                    PromptCase{"OtherAutomatic", "stub-1", true},
                    PromptCase{"OtherManual", "stub-1", false}),
    [](const testing::TestParamInfo<PromptCase>& info) { return info.param.name; });

// 场景:历史的原生工具名被映射为模型别名。期望:主请求使用别名,压缩仍接收原生
// tool_calls 及原始结果正文;回归会把 model_facing 误套到压缩,或改写工具输出的数据。
TEST(AgentLoopContextGolden, CompactionReceivesNativeHistoryWhileMainRequestUsesModelAliases) {
    Isolation isolation;
    ScopedModelToolNameMappings mapped{{"file_read", "inspect_document"}};
    auto provider = std::make_shared<CompactProbe>();
    Harness h(isolation, "raw-history", provider);
    h.tools.register_tool(h.probe("file_read", true));
    h.loop->set_context_window(1000000);
    const std::string original_output = "literal file_read data\n中文原始输出\ninspect_document is also data";
    auto call = message("assistant", "inspect the file");
    call.tool_calls = Json::array({{{"id", "historical-read"}, {"type", "function"},
        {"function", {{"name", "file_read"}, {"arguments", "{\"path\":\"original.txt\"}"}}}}});
    auto result = message("tool", original_output);
    result.tool_call_id = "historical-read";
    h.loop->push_message(message("user", "read the original document"));
    h.loop->push_message(call);
    h.loop->push_message(result);
    h.loop->push_message(message("assistant", "previous answer"));
    provider->push_text("new answer");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("continue before compact"); }));
    const auto main = provider->messages_for_turn(0);
    const auto find_call = [](const std::vector<ChatMessage>& messages) {
        return std::find_if(messages.begin(), messages.end(), [](const ChatMessage& item) {
            return item.tool_calls.is_array() && !item.tool_calls.empty();
        });
    };
    const auto main_call = find_call(main);
    ASSERT_NE(main_call, main.end());
    EXPECT_EQ(main_call->tool_calls[0]["function"]["name"], "inspect_document");
    const auto raw = h.loop->messages();
    const auto raw_call = find_call(raw);
    ASSERT_NE(raw_call, raw.end());
    EXPECT_EQ(raw_call->tool_calls, call.tool_calls);
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit_compact(); }));
    std::lock_guard<std::mutex> lock(provider->mutex);
    ASSERT_EQ(provider->compact_requests.size(), 1u);
    const auto& compact = provider->compact_requests[0];
    const auto compact_call = find_call(compact);
    ASSERT_NE(compact_call, compact.end());
    EXPECT_EQ(compact_call->tool_calls, call.tool_calls);
    EXPECT_EQ(compact_call->content, call.content);
    const auto compact_result = std::find_if(compact.begin(), compact.end(), [](const ChatMessage& item) {
        return item.role == "tool" && item.tool_call_id == "historical-read";
    });
    ASSERT_NE(compact_result, compact.end());
    EXPECT_EQ(compact_result->content, original_output);
    const auto main_result = std::find_if(main.begin(), main.end(), [](const ChatMessage& item) {
        return item.role == "tool" && item.tool_call_id == "historical-read";
    });
    ASSERT_NE(main_result, main.end());
    EXPECT_EQ(main_result->content, original_output);
}
} // namespace
