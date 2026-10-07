#include "test_support/agent_loop/characterization_fixture.hpp"
#include "agent/compaction/compact_prompt.hpp"
#include "session/request_context_record.hpp"
#include "session/session_serializer.hpp"
#include "tool/todo_write_tool.hpp"

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
        compact_tools.push_back(tools);
        ChatResponse response;
        response.content = "Golden compact summary.";
        response.finish_reason = "stop";
        return response;
    }
    bool supports_compaction_prefix_reuse() const override { return prefix_capability; }
    ChatResponse chat_for_compaction(const std::vector<ChatMessage>& messages,
                                    const std::vector<ToolDef>& tools,
                                    const std::atomic<bool>* abort_flag) override {
        return chat_cancellable(messages, tools, abort_flag);
    }
    bool prefix_capability = false;
    std::mutex mutex;
    std::vector<std::vector<ChatMessage>> compact_requests;
    std::vector<std::size_t> compact_tool_counts;
    std::vector<std::vector<ToolDef>> compact_tools;
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

class AgentLoopCompactionPrefix : public testing::TestWithParam<PromptCase> {};

// Exercise controller wiring, not just compact_messages: automatic/manual
// compaction must preserve every previously sent model-facing message and
// schema, including aliases, structured attachments and reasoning history.
TEST_P(AgentLoopCompactionPrefix, ReusesCompleteMainRequestBeforeSummaryInstruction) {
    Isolation isolation;
    ScopedModelToolNameMappings mappings{{"file_read", "InspectDocument"}};
    auto provider = std::make_shared<CompactProbe>();
    provider->prefix_capability = true;
    provider->set_model(GetParam().model);
    Harness h(isolation, GetParam().name, provider);
    h.tools.register_tool(h.probe("file_read", true));
    h.loop->set_context_window(1000000);
    h.permissions.set_mode(PermissionMode::Plan);
    h.install_hooks({"UserPromptSubmit", "PreCompact"}, [](const Json& payload) {
        const std::string context = payload.value("hook_event_name", "") == "PreCompact"
            ? "precompact queued hook"
            : (payload.value("prompt", "") == "pending new input"
                ? "pending user hook" : "existing request hook");
        return Json{{"hookSpecificOutput", {{"additionalContext", context}}}};
    });
    auto user = message("user", "read original attachment");
    user.content_parts = Json::array({
        {{"type", "text"}, {"text", user.content}},
        {{"type", "file"}, {"path", "/tmp/original.txt"}},
    });
    auto call = message("assistant", "inspect the document");
    call.reasoning_content = "Nonempty reasoning retained for GLM and DeepSeek.";
    call.tool_calls = Json::array({{{"id", "historical-read"}, {"type", "function"},
        {"function", {{"name", "file_read"}, {"arguments", "{\"path\":\"original.txt\"}"}}}}});
    auto output = message("tool", "literal file_read output is unchanged");
    output.tool_call_id = "historical-read";
    h.loop->push_message(user);
    h.loop->push_message(call);
    h.loop->push_message(output);

    StreamEvent answer;
    answer.type = StreamEventType::Delta;
    answer.content = "completed before compaction";
    StreamEvent usage;
    usage.type = StreamEventType::Usage;
    usage.usage.has_data = true;
    usage.usage.prompt_tokens = GetParam().automatic ? 950000 : 100;
    usage.usage.total_tokens = usage.usage.prompt_tokens + 10;
    StreamEvent done;
    done.type = StreamEventType::Done;
    provider->push_events({answer, usage, done});
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("finish the inspection"); }));
    const auto main = provider->messages_for_turn(0);
    const auto main_tools = provider->tools_for_turn(0);
    ASSERT_FALSE(main.empty());
    ASSERT_FALSE(main_tools.empty());
    const auto contains = [](const std::vector<ChatMessage>& messages, const std::string& text) {
        return std::any_of(messages.begin(), messages.end(), [&](const ChatMessage& item) {
            return item.content.find(text) != std::string::npos;
        });
    };
    EXPECT_TRUE(contains(main, "<plan_mode>"));
    EXPECT_TRUE(contains(main, "existing request hook"));
    if (GetParam().automatic) {
        provider->push_text("continued after compaction");
        ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("pending new input"); }));
    } else {
        ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit_compact(); }));
        provider->push_text("continued after manual compaction");
        ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("after manual compaction"); }));
    }
    const auto continued = provider->messages_for_turn(1);
    EXPECT_TRUE(contains(continued, "precompact queued hook"));
    if (GetParam().automatic) EXPECT_TRUE(contains(continued, "pending user hook"));

    std::lock_guard<std::mutex> lock(provider->mutex);
    ASSERT_EQ(provider->compact_requests.size(), 1u);
    const auto& compact = provider->compact_requests[0];
    ASSERT_EQ(compact.size(), main.size() + 2u);
    for (std::size_t i = 0; i < main.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(compact[i].role, main[i].role);
        EXPECT_EQ(compact[i].content, main[i].content);
        EXPECT_EQ(compact[i].content_parts, main[i].content_parts);
        EXPECT_EQ(compact[i].reasoning_content, main[i].reasoning_content);
        EXPECT_EQ(compact[i].tool_calls, main[i].tool_calls);
        EXPECT_EQ(compact[i].tool_call_id, main[i].tool_call_id);
    }
    EXPECT_EQ(compact[main.size()].content, "completed before compaction");
    EXPECT_EQ(compact.back().content, get_compact_prompt());
    EXPECT_FALSE(contains(compact, "precompact queued hook"));
    EXPECT_FALSE(contains(compact, "pending user hook"));
    const auto compact_call = std::find_if(compact.begin(), compact.end(), [](const ChatMessage& item) {
        return item.tool_calls.is_array() && !item.tool_calls.empty();
    });
    ASSERT_NE(compact_call, compact.end());
    EXPECT_EQ(compact_call->tool_calls[0]["function"]["name"], "InspectDocument");
    EXPECT_EQ(compact_call->reasoning_content, call.reasoning_content);
    ASSERT_EQ(provider->compact_tools[0].size(), main_tools.size());
    for (std::size_t i = 0; i < main_tools.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(provider->compact_tools[0][i].name, main_tools[i].name);
        EXPECT_EQ(provider->compact_tools[0][i].description, main_tools[i].description);
        EXPECT_EQ(provider->compact_tools[0][i].parameters, main_tools[i].parameters);
    }
    for (const auto& item : compact) EXPECT_NE(item.content, "pending new input");
}

INSTANTIATE_TEST_SUITE_P(CacheStability, AgentLoopCompactionPrefix,
    testing::Values(PromptCase{"GlmAutomatic", "glm-4.6", true},
                    PromptCase{"GlmManual", "glm-4.6", false},
                    PromptCase{"DeepSeekAutomatic", "deepseek-reasoner", true},
                    PromptCase{"DeepSeekManual", "deepseek-reasoner", false},
                    PromptCase{"GptAutomatic", "gpt-5", true},
                    PromptCase{"GptManual", "gpt-5", false}),
    [](const testing::TestParamInfo<PromptCase>& info) { return info.param.name; });

TEST(AgentLoopContextGolden, FailedCheckpointAppendKeepsHistoryAndWindowUntilDurableRetry) {
    Isolation isolation;
    auto provider = std::make_shared<CompactProbe>();
    provider->prefix_capability = true;
    Harness h(isolation, "failed-compact-append", provider);
    h.loop->set_context_window(1000000);
    provider->push_text("initial answer");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("task that must survive"); }));
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit_compact(); }));
    const auto checkpoint_before = h.session->load_latest_compact_checkpoint();
    ASSERT_TRUE(checkpoint_before.has_value());
    provider->push_text("new detailed answer not yet summarized");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("new work after checkpoint"); }));
    const auto history_before = h.loop->messages();
    std::size_t compaction_calls_before = 0;
    {
        std::lock_guard<std::mutex> lock(provider->mutex);
        compaction_calls_before = provider->compact_requests.size();
    }
    std::size_t events_before = 0;
    {
        std::lock_guard<std::mutex> lock(h.observed->mutex);
        events_before = h.observed->events.size();
    }

    const auto path = std::filesystem::path(SessionStorage::session_path(
        h.session->current_project_dir(), h.session->current_session_id()));
    const auto relative = std::filesystem::canonical(path).lexically_relative(
        std::filesystem::canonical(isolation.directory.path));
    ASSERT_FALSE(relative.empty());
    ASSERT_FALSE(relative.is_absolute());
    ASSERT_NE(*relative.begin(), std::filesystem::path(".."));
    const auto saved = path.string() + ".saved-for-append-failure";
    ASSERT_FALSE(std::filesystem::exists(saved));
    std::filesystem::rename(path, saved);
    ASSERT_TRUE(std::filesystem::create_directory(path));

    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit_compact(); }));
    EXPECT_FALSE(h.loop->is_busy());
    {
        std::lock_guard<std::mutex> lock(provider->mutex);
        EXPECT_EQ(provider->compact_requests.size(), compaction_calls_before + 1);
    }
    EXPECT_EQ(h.session->last_error(), "failed to append compact checkpoint");
    const auto history_after = h.loop->messages();
    ASSERT_EQ(history_after.size(), history_before.size());
    for (std::size_t i = 0; i < history_before.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(serialize_message(history_after[i]), serialize_message(history_before[i]));
    }
    bool saw_error = false;
    bool finished_busy = false;
    {
        std::lock_guard<std::mutex> lock(h.observed->mutex);
        for (std::size_t i = events_before; i < h.observed->events.size(); ++i) {
            const auto& event = h.observed->events[i];
            if (event.kind == SessionEventKind::BusyChanged && !event.payload.value("busy", true)) {
                finished_busy = true;
            }
            if (event.kind != SessionEventKind::Message) continue;
            saw_error = saw_error || event.payload.value("role", "") == "error";
            const auto text = event.payload.value("content", std::string{});
            EXPECT_EQ(text.find("[Compact Checkpoint]"), std::string::npos);
            EXPECT_EQ(text.find("[Conversation summary]"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_error);
    EXPECT_TRUE(finished_busy);

    ASSERT_TRUE(std::filesystem::remove(path));
    std::filesystem::rename(saved, path);
    const auto persisted_after_failure = h.session->load_latest_compact_checkpoint();
    ASSERT_TRUE(persisted_after_failure.has_value());
    EXPECT_EQ(persisted_after_failure->window_number, checkpoint_before->window_number);
    EXPECT_EQ(persisted_after_failure->window_id, checkpoint_before->window_id);
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit_compact(); }));
    const auto retried = h.session->load_latest_compact_checkpoint();
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(retried->window_number, checkpoint_before->window_number + 1);
    EXPECT_EQ(retried->previous_window_id, checkpoint_before->window_id);
    EXPECT_EQ(retried->first_window_id, checkpoint_before->first_window_id);
    EXPECT_NE(retried->window_id, checkpoint_before->window_id);
    EXPECT_FALSE(h.loop->is_busy());
}

TEST(AgentLoopContextGolden, FailedSnapshotAppendStopsBeforeProviderWithoutPhantomContext) {
    Isolation isolation;
    auto provider = std::make_shared<CompactProbe>();
    Harness h(isolation, "failed-request-snapshot-append", provider);
    h.loop->set_context_window(1000000);
    ASSERT_TRUE(h.session->try_on_message(message("user", "existing persisted transcript")));
    const auto path = std::filesystem::path(SessionStorage::session_path(
        h.session->current_project_dir(), h.session->current_session_id()));
    const auto relative = std::filesystem::canonical(path).lexically_relative(
        std::filesystem::canonical(isolation.directory.path));
    ASSERT_FALSE(relative.empty());
    ASSERT_FALSE(relative.is_absolute());
    ASSERT_NE(*relative.begin(), std::filesystem::path(".."));
    const auto saved = path.string() + ".saved-for-snapshot-failure";
    ASSERT_FALSE(std::filesystem::exists(saved));
    std::filesystem::rename(path, saved);
    ASSERT_TRUE(std::filesystem::create_directory(path));

    provider->push_text("must not be requested before context is durable");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("first request needs a snapshot"); }));
    EXPECT_EQ(provider->turn_count(), 0);
    EXPECT_FALSE(h.loop->is_busy());
    for (const auto& item : h.loop->messages()) {
        EXPECT_FALSE(is_request_context_record(item));
    }
    bool saw_context_error = false;
    bool finished_busy = false;
    {
        std::lock_guard<std::mutex> lock(h.observed->mutex);
        for (const auto& event : h.observed->events) {
            if (event.kind == SessionEventKind::BusyChanged && !event.payload.value("busy", true)) {
                finished_busy = true;
            }
            if (event.kind == SessionEventKind::Message && event.payload.value("role", "") == "error") {
                saw_context_error = saw_context_error ||
                    event.payload.value("content", std::string{}).find(
                        "Could not persist request context; request was not sent.") != std::string::npos;
            }
        }
    }
    EXPECT_TRUE(saw_context_error);
    EXPECT_TRUE(finished_busy);

    ASSERT_TRUE(std::filesystem::remove(path));
    std::filesystem::rename(saved, path);
    const auto persisted = h.session->load_active_messages();
    ASSERT_EQ(persisted.size(), 1u);
    EXPECT_EQ(persisted.front().content, "existing persisted transcript");
}

TEST(AgentLoopContextGolden, TodoWriteChecklistIsRestoredOnceInDurableCompactWindow) {
    Isolation isolation;
    auto provider = std::make_shared<CompactProbe>();
    provider->prefix_capability = true;
    Harness h(isolation, "compact-todo-checklist", provider);
    h.loop->set_context_window(1000000);
    ASSERT_TRUE(h.tools.register_tool(create_todo_write_tool()));
    provider->push_tool_call("TodoWrite", Json{{"todos", Json::array({
        {{"id", "done"}, {"content", "UNIQUE_COMPLETED_CHECKLIST_ITEM"}, {"status", "completed"}},
        {{"id", "active"}, {"content", "UNIQUE_ACTIVE_CHECKLIST_ITEM"}, {"status", "in_progress"}},
        {{"id", "later"}, {"content", "UNIQUE_PENDING_CHECKLIST_ITEM"}, {"status", "pending"}},
        {{"id", "cancelled"}, {"content", "UNIQUE_CANCELLED_CHECKLIST_ITEM"}, {"status", "cancelled"}},
    })}}.dump(), "save-checklist");
    provider->push_text("checklist saved");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("track the work"); }));
    const auto todos = h.session->current_todos();
    ASSERT_EQ(todos.size(), 4u);
    const auto complete_list = format_todo_injection(todos);
    ASSERT_FALSE(complete_list.empty());
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit_compact(); }));
    const auto checkpoint = h.session->load_latest_compact_checkpoint();
    ASSERT_TRUE(checkpoint.has_value());
    ASSERT_FALSE(checkpoint->replacement_history.empty());
    const auto& snapshot = checkpoint->replacement_history.front();
    EXPECT_TRUE(is_request_context_snapshot(snapshot));
    EXPECT_NE(snapshot.content.find(complete_list), std::string::npos);
    EXPECT_EQ(checkpoint->replacement_history.back().content,
        get_compact_summary_prefix() + "\nGolden compact summary.");
    const auto reloaded = reconstruct_effective_model_history(h.session->load_active_messages());
    ASSERT_FALSE(reloaded.empty());
    EXPECT_EQ(serialize_message(reloaded.front()), serialize_message(snapshot));

    provider->push_text("continued once");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("continue the checklist"); }));
    provider->push_text("continued twice");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("continue again"); }));
    for (int turn : {2, 3}) {
        const auto request = provider->messages_for_turn(turn);
        ASSERT_FALSE(request.empty());
        for (const auto& todo : todos) {
            EXPECT_EQ(std::count_if(request.begin(), request.end(), [&](const ChatMessage& item) {
                return item.content.find(todo.content) != std::string::npos;
            }), 1) << todo.id;
        }
    }
}

// Legacy providers still omit tools, but consume the same model-facing context
// projection as normal requests. Stored tool names and result data stay intact.
TEST(AgentLoopContextGolden, ToolFreeCompactionUsesModelAliasesWithoutRewritingStoredHistory) {
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
    EXPECT_EQ(compact_call->tool_calls, main_call->tool_calls);
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
