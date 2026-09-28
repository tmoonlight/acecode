#include <gtest/gtest.h>

#include "commands/compact.hpp"
#include "commands/compact_prompt.hpp"

#include <deque>
#include <stdexcept>
#include <thread>

namespace {

class ChatStubProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>& messages,
        const std::vector<acecode::ToolDef>&) override {
        calls.push_back(messages);
        if (!exceptions.empty()) {
            const std::string error = exceptions.front();
            exceptions.pop_front();
            throw std::runtime_error(error);
        }
        if (!responses.empty()) {
            auto response = responses.front();
            responses.pop_front();
            return response;
        }
        acecode::ChatResponse response;
        response.content = "Important retained context.";
        response.finish_reason = "stop";
        return response;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback&,
                     std::atomic<bool>* = nullptr) override {}

    std::string name() const override { return "stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "stub"; }
    void set_model(const std::string&) override {}
    bool supports_native_compaction() const override {
        return native_capability;
    }

    static acecode::ChatResponse response(std::string content,
                                          std::string finish_reason = "stop") {
        acecode::ChatResponse out;
        out.content = std::move(content);
        out.finish_reason = std::move(finish_reason);
        return out;
    }

    std::vector<std::vector<acecode::ChatMessage>> calls;
    std::deque<acecode::ChatResponse> responses;
    std::deque<std::string> exceptions;
    bool native_capability = false;
};

acecode::ChatMessage msg(std::string role,
                         std::string content,
                         std::string uuid = {}) {
    acecode::ChatMessage out;
    out.role = std::move(role);
    out.content = std::move(content);
    out.uuid = std::move(uuid);
    return out;
}

acecode::ChatMessage tool_call_message(const std::string& id) {
    auto out = msg("assistant", "");
    out.tool_calls = nlohmann::json::array({
        {
            {"id", id},
            {"type", "function"},
            {"function", {{"name", "probe"}, {"arguments", "{}"}}},
        },
    });
    return out;
}

acecode::ChatMessage tool_output_message(const std::string& id) {
    auto out = msg("tool", "probe output");
    out.tool_call_id = id;
    return out;
}

acecode::ChatResponse provider_error_response(
    acecode::ProviderErrorKind kind,
    int status_code,
    std::string message,
    bool retryable,
    std::string raw_body = {},
    std::int64_t server_retry_after_ms = 0) {
    auto response = ChatStubProvider::response(message, "error");
    response.provider_error.kind = kind;
    response.provider_error.status_code = status_code;
    response.provider_error.display_message = std::move(message);
    response.provider_error.raw_body = std::move(raw_body);
    response.provider_error.retryable = retryable;
    response.provider_error.server_retry_after_ms =
        server_retry_after_ms;
    return response;
}

void expect_same_request(const std::vector<acecode::ChatMessage>& lhs,
                         const std::vector<acecode::ChatMessage>& rhs) {
    ASSERT_EQ(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        EXPECT_EQ(lhs[i].role, rhs[i].role) << "message index " << i;
        EXPECT_EQ(lhs[i].content, rhs[i].content) << "message index " << i;
        EXPECT_EQ(lhs[i].tool_call_id, rhs[i].tool_call_id)
            << "message index " << i;
        EXPECT_EQ(lhs[i].tool_calls, rhs[i].tool_calls)
            << "message index " << i;
    }
}

} // namespace

// 注意:这里比较的是 get_compact_prompt() 的返回值。提示词从 fix-feedback-0924
// 起在末尾有意偏离 Codex 原文(追加「Output requirements」一段,禁止调工具 /
// 输出调用标签),见 PromptForbidsToolCallsButAllowsQuotingCommands。
TEST(CompactCore, UsesExactCodexPromptAndSummaryShape) {
    ChatStubProvider provider;
    std::vector<acecode::ChatMessage> initial_context{
        msg("system", "stable base instructions"),
    };
    auto first_answer = tool_call_message("call-1");
    first_answer.content = "first answer";
    std::vector<acecode::ChatMessage> messages{
        msg("user", "first request", "u1"),
        std::move(first_answer),
        tool_output_message("call-1"),
        msg("user", "latest request", "u2"),
    };

    auto result = acecode::compact_messages(
        provider, messages, initial_context, false, nullptr);

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 1u);
    const auto& request = provider.calls.front();
    ASSERT_EQ(request.size(), initial_context.size() + messages.size() + 1);
    EXPECT_EQ(request.front().content, "stable base instructions");
    EXPECT_EQ(request.back().role, "user");
    EXPECT_EQ(request.back().content, acecode::get_compact_prompt());
    EXPECT_EQ(request[2].role, "assistant");
    EXPECT_EQ(request[3].role, "tool");

    ASSERT_EQ(result.compacted_messages.size(), 3u);
    EXPECT_EQ(result.compacted_messages[0].content, "first request");
    EXPECT_EQ(result.compacted_messages[1].content, "latest request");
    EXPECT_EQ(result.compacted_messages[2].role, "user");
    EXPECT_TRUE(result.compacted_messages[2].is_compact_summary);
    EXPECT_EQ(
        result.compacted_messages[2].content,
        acecode::get_compact_summary_prefix() +
            "\nImportant retained context.");
    EXPECT_EQ(result.summary_text, "Important retained context.");
}

TEST(CompactCore, EmptyHistoryStillRunsCheckpointPrompt) {
    ChatStubProvider provider;

    auto result = acecode::compact_messages(provider, {});

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 1u);
    ASSERT_EQ(provider.calls[0].size(), 1u);
    EXPECT_EQ(provider.calls[0][0].content, acecode::get_compact_prompt());
    ASSERT_EQ(result.compacted_messages.size(), 1u);
    EXPECT_EQ(result.compacted_messages[0].content,
              acecode::get_compact_summary_prefix() +
                  "\nImportant retained context.");
}

TEST(CompactCore, IncompleteNativeCapabilityFallsBackToValidatedLocalPath) {
    ChatStubProvider provider;
    provider.native_capability = true;

    auto result = acecode::compact_messages(
        provider, {msg("user", "keep this request")});

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 1u);
    EXPECT_EQ(provider.calls[0].back().content, acecode::get_compact_prompt());
    EXPECT_EQ(result.compacted_messages.back().content,
              acecode::get_compact_summary_prefix() +
                  "\nImportant retained context.");
}

TEST(CompactCore, RetainsNewestUserTextWithinTwentyThousandTokenBudget) {
    const std::string old_user(40000, 'a');   // 10,000 approximate tokens
    const std::string new_user(60000, 'b');   // 15,000 approximate tokens
    std::vector<acecode::ChatMessage> messages{
        msg("user", old_user, "old"),
        msg("assistant", "answer"),
        msg("user", new_user, "new"),
    };

    auto compacted = acecode::build_compacted_history(messages, "summary");

    ASSERT_EQ(compacted.size(), 3u);
    EXPECT_EQ(compacted[0].uuid, "old");
    EXPECT_NE(compacted[0].content.find("5000 tokens truncated"),
              std::string::npos);
    EXPECT_EQ(compacted[0].content.substr(0, 32), std::string(32, 'a'));
    EXPECT_EQ(compacted[0].content.substr(compacted[0].content.size() - 32),
              std::string(32, 'a'));
    EXPECT_EQ(compacted[1].uuid, "new");
    EXPECT_EQ(compacted[1].content, new_user);
    EXPECT_EQ(compacted[2].content,
              acecode::get_compact_summary_prefix() + "\nsummary");
}

// 场景:用户只发了一张图片(content 为空,content_parts 只有 image),随后发生压缩。
// 期望:保留下来的这条 user 消息带一句占位说明,不是空内容;纯空消息直接跳过。
// 回归表现:压缩后留下一条空内容的 user 消息,切到 agnes-3.0-flash 后每次请求都被
// 400「message content cannot be empty」拒绝(yubo2 反馈)。
TEST(CompactCore, ImageOnlyUserMessageKeepsPlaceholderText) {
    auto image_only = msg("user", "", "img");
    image_only.content_parts = nlohmann::json::array({
        {{"type", "image_url"}, {"image_url", "data:image/png;base64,abc"}},
    });
    std::vector<acecode::ChatMessage> messages{
        std::move(image_only),
        msg("user", "   "),
        msg("user", "follow-up question"),
    };

    auto compacted = acecode::build_compacted_history(messages, "summary");

    ASSERT_EQ(compacted.size(), 3u);
    EXPECT_EQ(compacted[0].content,
              "[Image attachment omitted during context compaction]");
    EXPECT_TRUE(compacted[0].content_parts.empty());
    EXPECT_EQ(compacted[1].content, "follow-up question");
}

TEST(CompactCore, ExcludesPriorSummaryAndNonUserItems) {
    auto previous_summary = msg(
        "user",
        acecode::get_compact_summary_prefix() + "\nold summary");
    previous_summary.is_compact_summary = true;
    auto structured_user = msg("user", "real request", "real");
    structured_user.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", "real request"}},
        {{"type", "image_url"}, {"image_url", "data:image/png;base64,abc"}},
    });
    std::vector<acecode::ChatMessage> messages{
        std::move(previous_summary),
        msg("assistant", "assistant detail"),
        msg("tool", "tool detail"),
        std::move(structured_user),
    };

    auto compacted = acecode::build_compacted_history(messages, "new summary");

    ASSERT_EQ(compacted.size(), 2u);
    EXPECT_EQ(compacted[0].content, "real request");
    EXPECT_TRUE(compacted[0].content_parts.empty());
    EXPECT_EQ(compacted[1].content,
              acecode::get_compact_summary_prefix() + "\nnew summary");
}

// 触发场景:用户消息带「粘贴的文本」文件块(content_parts = [text, file]),
// 以及只有文件块、编辑器里什么都没打的消息(content 为空,只有 file 部件),
// 之后会话被压缩。
// 期望行为:保留下来的 user 消息 content_parts 变为 [text(= content), file],
// 只有文件块的消息保留为 [file](不补空 text 部件);其它结构化部件(图片、
// 浏览器上下文)照旧丢弃。
// 回归:压缩一律把 content_parts 清空,provider 只剩 content,文件引用连同
// read_path 一起消失,压缩后模型再也读不到用户粘贴的大段材料。
TEST(CompactCore, RetainedUserMessageKeepsFileReferenceParts) {
    const nlohmann::json file_part = {
        {"type", "file"},
        {"attachment", {
            {"id", "att_paste"},
            {"session_id", "session-a"},
            {"name", "粘贴的文本.txt"},
            {"kind", "file"},
            {"mime_type", "text/plain"},
            {"path", "C:/acecode/attachments/session-a/att_paste.txt"},
            {"size_bytes", 400000},
            {"metadata", {{"origin", "pasted_text"}}},
        }},
    };
    auto with_text = msg("user", "分析这份日志", "with-text");
    with_text.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", "分析这份日志"}},
        file_part,
        {{"type", "image"}, {"attachment", {{"id", "att_image"}}}},
        {{"type", "browser_context"}, {"context", {{"url", "https://x"}}}},
    });
    auto file_only = msg("user", "", "file-only");
    file_only.content_parts = nlohmann::json::array({file_part});
    std::vector<acecode::ChatMessage> messages{
        std::move(with_text),
        msg("assistant", "done"),
        std::move(file_only),
    };

    auto compacted = acecode::build_compacted_history(messages, "summary");

    ASSERT_EQ(compacted.size(), 3u);
    EXPECT_EQ(compacted[0].uuid, "with-text");
    const nlohmann::json expected_with_text = nlohmann::json::array({
        {{"type", "text"}, {"text", "分析这份日志"}},
        file_part,
    });
    EXPECT_EQ(compacted[0].content_parts, expected_with_text);
    EXPECT_EQ(compacted[1].uuid, "file-only");
    EXPECT_EQ(compacted[1].content_parts, nlohmann::json::array({file_part}));
    EXPECT_TRUE(compacted[2].is_compact_summary);
}

TEST(CompactCore, ExcludesInternalUserContextRows) {
    auto internal = [](const char* key, const char* content) {
        auto message = msg("user", content);
        message.metadata = nlohmann::json{{key, true}};
        return message;
    };
    std::vector<acecode::ChatMessage> messages{
        msg("user", "real request", "real"),
        internal("hidden_goal_context", "goal steering"),
        internal("hidden_plan_mode_context", "plan instructions"),
        internal("hidden_todo_context", "todo injection"),
        internal("hidden_hook_stop_continuation", "hook continuation"),
        internal("compact_initial_context", "rebuilt session context"),
        internal("transcript_only", "human transcript marker"),
    };

    auto compacted = acecode::build_compacted_history(messages, "summary");

    ASSERT_EQ(compacted.size(), 2u);
    EXPECT_EQ(compacted[0].uuid, "real");
    EXPECT_EQ(compacted[0].content, "real request");
    EXPECT_EQ(compacted[1].content,
              acecode::get_compact_summary_prefix() + "\nsummary");
}

TEST(CompactCore, InsertsMutableContextBeforeLastRealUserAndKeepsSummaryFinal) {
    auto summary = msg(
        "user", acecode::get_compact_summary_prefix() + "\nsummary");
    summary.is_compact_summary = true;
    std::vector<acecode::ChatMessage> messages{
        msg("user", "older request"),
        msg("user", "active request"),
        std::move(summary),
    };
    std::vector<acecode::ChatMessage> context{
        msg("user", "session context"),
        msg("user", "request context"),
    };

    acecode::insert_context_before_last_real_user_or_summary(
        messages, std::move(context));

    ASSERT_EQ(messages.size(), 5u);
    EXPECT_EQ(messages[0].content, "older request");
    EXPECT_EQ(messages[1].content, "session context");
    EXPECT_EQ(messages[2].content, "request context");
    EXPECT_EQ(messages[3].content, "active request");
    EXPECT_EQ(messages[4].content,
              acecode::get_compact_summary_prefix() + "\nsummary");
}

TEST(CompactCore, InsertsMutableContextBeforeSummaryWhenNoRealUserRemains) {
    auto summary = msg(
        "user", acecode::get_compact_summary_prefix() + "\nsummary");
    summary.is_compact_summary = true;
    std::vector<acecode::ChatMessage> messages{std::move(summary)};

    acecode::insert_context_before_last_real_user_or_summary(
        messages, {msg("user", "session context")});

    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].content, "session context");
    EXPECT_EQ(messages[1].content,
              acecode::get_compact_summary_prefix() + "\nsummary");
}

TEST(CompactCore, ContextOverflowRemovesOneOldestHistoryItemPerRetry) {
    ChatStubProvider provider;
    provider.responses.push_back(ChatStubProvider::response(
        "maximum context length exceeded", "error"));
    provider.responses.push_back(ChatStubProvider::response("summary"));
    std::vector<acecode::ChatMessage> initial_context{
        msg("system", "stable"),
    };
    std::vector<acecode::ChatMessage> messages{
        msg("user", "oldest"),
        msg("assistant", "middle"),
        msg("user", "newest"),
    };

    auto result = acecode::compact_messages(
        provider, messages, initial_context, true, nullptr);

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 2u);
    EXPECT_EQ(provider.calls[0].size(), 5u);
    EXPECT_EQ(provider.calls[1].size(), 4u);
    EXPECT_EQ(provider.calls[0][0].content, "stable");
    EXPECT_EQ(provider.calls[1][0].content, "stable");
    EXPECT_EQ(provider.calls[0].back().content, acecode::get_compact_prompt());
    EXPECT_EQ(provider.calls[1].back().content, acecode::get_compact_prompt());
    EXPECT_EQ(provider.calls[0][1].content, "oldest");
    EXPECT_EQ(provider.calls[1][1].content, "middle");
    EXPECT_EQ(result.compaction_request_items_removed, 1);
}

TEST(CompactCore, OverflowWithOnlyPromptFailsWithoutReplacement) {
    ChatStubProvider provider;
    provider.responses.push_back(ChatStubProvider::response(
        "context_length_exceeded", "error"));

    auto result = acecode::compact_messages(provider, {});

    EXPECT_FALSE(result.performed);
    EXPECT_TRUE(result.compacted_messages.empty());
    EXPECT_NE(result.error.find("no removable history item"), std::string::npos);
}

TEST(CompactCore, ContextOverflowExceptionUsesSameOneItemRetry) {
    ChatStubProvider provider;
    provider.exceptions.push_back("prompt is too long");
    provider.responses.push_back(ChatStubProvider::response("summary"));
    std::vector<acecode::ChatMessage> messages{
        msg("user", "oldest"),
        msg("assistant", "newest"),
    };

    auto result = acecode::compact_messages(provider, messages);

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 2u);
    EXPECT_EQ(provider.calls[0].size(), 3u);
    EXPECT_EQ(provider.calls[1].size(), 2u);
    EXPECT_EQ(provider.calls[1][0].content, "newest");
}

TEST(CompactCore, OverflowRetryRemovesMatchingToolOutputWithOldestCall) {
    ChatStubProvider provider;
    provider.responses.push_back(ChatStubProvider::response(
        "maximum context length exceeded", "error"));
    provider.responses.push_back(ChatStubProvider::response(
        "maximum context length exceeded", "error"));
    provider.responses.push_back(ChatStubProvider::response("summary"));
    std::vector<acecode::ChatMessage> messages{
        msg("user", "old turn"),
        tool_call_message("call-old"),
        tool_output_message("call-old"),
        msg("user", "new turn"),
    };

    auto result = acecode::compact_messages(provider, messages);

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 3u);
    EXPECT_EQ(provider.calls[0].size(), 5u);
    EXPECT_EQ(provider.calls[1].size(), 4u);
    ASSERT_EQ(provider.calls[2].size(), 2u);
    EXPECT_EQ(provider.calls[2][0].content, "new turn");
    EXPECT_EQ(provider.calls[2][1].content, acecode::get_compact_prompt());
    EXPECT_EQ(result.compaction_request_items_removed, 3);
}

// 场景:vLLM / LiteLLM 转发的上下文超限原文(yubo2 切到 agnes-2.5-flash 后)。
// 期望:认作上下文超限,压缩走「删最旧历史再重试」。
// 回归表现:认不出时压缩退化成机械裁剪,一次丢掉 527 条消息且没有摘要。
TEST(CompactCore, ContextOverflowRecognizesLiteLlmContextWindowExceeded) {
    acecode::ProviderErrorInfo info;
    info.kind = acecode::ProviderErrorKind::Http;
    info.status_code = 400;
    info.raw_body =
        R"({"error":{"message":"litellm.ContextWindowExceededError: litellm.BadRequestError: ContextWindowExceededError: OpenAIException - {\"object\":\"error\",\"message\":\"The input (564686 tokens) is longer than the model's context length (524288 tokens).\",\"type\":\"BadRequestError\",\"param\":null,\"code\":400}"}})";
    EXPECT_TRUE(acecode::is_context_overflow_error(info));
    EXPECT_TRUE(acecode::is_context_overflow_error(
        "The input (564686 tokens) is longer than the model's context length (524288 tokens)."));
}

// 场景:换到窗口更小的模型后压缩,被总结的历史远超新窗口,服务端连续报超限。
// 期望:前 3 次每次只删最旧的一条(尽量多保留可总结的内容),第 4 次起每次至少
// 删掉约 1/4 的估算 token,重试次数有上限。
// 回归表现:修复前每次只删一条,几百条旧历史要来回请求几百次。
TEST(CompactCore, RepeatedOverflowShrinksHistoryGeometrically) {
    ChatStubProvider provider;
    for (int i = 0; i < 4; ++i) {
        provider.responses.push_back(ChatStubProvider::response(
            "maximum context length exceeded", "error"));
    }
    provider.responses.push_back(ChatStubProvider::response("summary"));
    std::vector<acecode::ChatMessage> messages;
    for (int i = 0; i < 40; ++i) {
        messages.push_back(msg(i % 2 == 0 ? "user" : "assistant",
                               "turn " + std::to_string(i) + std::string(200, 'x')));
    }

    auto result = acecode::compact_messages(provider, messages);

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 5u);
    // 每次请求 = 历史 + 压缩提示词;前 3 次重试各少一条。
    EXPECT_EQ(provider.calls[1].size(), provider.calls[0].size() - 1);
    EXPECT_EQ(provider.calls[2].size(), provider.calls[1].size() - 1);
    EXPECT_EQ(provider.calls[3].size(), provider.calls[2].size() - 1);
    // 第 4 次重试:37 条等长历史至少删掉 1/4(>= 10 条)。
    const std::size_t before = provider.calls[3].size() - 1;
    const std::size_t after = provider.calls[4].size() - 1;
    EXPECT_LE(after, before - before / 4);
    EXPECT_GT(after, 0u);
}

TEST(CompactCore, TerminalFailureDoesNotInstallHistory) {
    ChatStubProvider provider;
    provider.responses.push_back(
        ChatStubProvider::response("provider unavailable", "error"));
    std::vector<acecode::ChatMessage> messages{msg("user", "request")};

    auto result = acecode::compact_messages(provider, messages);

    EXPECT_FALSE(result.performed);
    EXPECT_EQ(result.error, "Summarization failed: provider unavailable");
    EXPECT_TRUE(result.compacted_messages.empty());
}

TEST(CompactCore, RetriesStructuredTransientErrorWithoutRemovingHistory) {
    ChatStubProvider provider;
    provider.responses.push_back(provider_error_response(
        acecode::ProviderErrorKind::Http,
        429,
        "rate limited",
        true,
        R"({"error":{"code":"rate_limit_exceeded"}})"));
    provider.responses.push_back(ChatStubProvider::response("summary"));
    std::vector<acecode::ChatMessage> messages{
        msg("user", "oldest"),
        msg("assistant", "newest"),
    };

    auto result = acecode::compact_messages(provider, messages);

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 2u);
    expect_same_request(provider.calls[0], provider.calls[1]);
    EXPECT_EQ(result.compaction_request_retries, 1);
    EXPECT_EQ(result.compaction_request_items_removed, 0);
}

TEST(CompactCore, TransientRetryContinuesUntilCancellation) {
    ChatStubProvider provider;
    for (int i = 0; i < 3; ++i) {
        provider.responses.push_back(provider_error_response(
            acecode::ProviderErrorKind::Http,
            503,
            "still overloaded",
            true));
    }
    std::vector<acecode::ChatMessage> messages{
        msg("user", "oldest"),
        msg("assistant", "newest"),
    };
    std::atomic<bool> abort_flag{false};
    int observed_retries = 0;

    auto result = acecode::compact_messages(
        provider,
        messages,
        {},
        false,
        &abort_flag,
        [&](const acecode::ProviderErrorInfo& info, bool waiting) {
            if (!waiting) return;
            ++observed_retries;
            EXPECT_EQ(info.retry_max_attempts, -1);
            if (observed_retries == 3) {
                abort_flag.store(true);
                provider.wake_retry_waiter();
            }
        });

    EXPECT_FALSE(result.performed);
    EXPECT_EQ(provider.calls.size(), 3u);
    expect_same_request(provider.calls[0], provider.calls[1]);
    expect_same_request(provider.calls[1], provider.calls[2]);
    EXPECT_EQ(observed_retries, 3);
    EXPECT_EQ(result.compaction_request_retries, 3);
    EXPECT_EQ(result.compaction_request_items_removed, 0);
    EXPECT_TRUE(result.compacted_messages.empty());
    EXPECT_EQ(result.error, "Compaction cancelled.");
}

TEST(CompactCore, CancellationInterruptsTransientRetryBackoff) {
    ChatStubProvider provider;
    provider.responses.push_back(provider_error_response(
        acecode::ProviderErrorKind::Timeout,
        0,
        "timed out",
        true,
        {},
        -1));
    std::atomic<bool> abort_flag{false};
    std::thread canceller([&abort_flag, &provider] {
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
        abort_flag.store(true);
        provider.wake_retry_waiter();
    });

    auto result = acecode::compact_messages(
        provider, {msg("user", "request")}, {}, false, &abort_flag);
    canceller.join();

    EXPECT_FALSE(result.performed);
    EXPECT_EQ(provider.calls.size(), 1u);
    EXPECT_EQ(result.compaction_request_retries, 1);
    EXPECT_EQ(result.error, "Compaction cancelled.");
}

TEST(CompactCore, ContextRemovalResetsTransientRetryBudget) {
    ChatStubProvider provider;
    provider.responses.push_back(provider_error_response(
        acecode::ProviderErrorKind::Timeout, 0, "timed out", true));
    provider.responses.push_back(provider_error_response(
        acecode::ProviderErrorKind::Http,
        400,
        "maximum context length exceeded",
        false,
        R"({"error":{"code":"context_length_exceeded"}})"));
    provider.responses.push_back(provider_error_response(
        acecode::ProviderErrorKind::Http, 503, "overloaded", true));
    provider.responses.push_back(ChatStubProvider::response("summary"));
    std::vector<acecode::ChatMessage> messages{
        msg("user", "oldest"),
        msg("assistant", "newest"),
    };

    std::vector<int> retry_attempts;
    auto result = acecode::compact_messages(
        provider,
        messages,
        {},
        false,
        nullptr,
        [&](const acecode::ProviderErrorInfo& info, bool waiting) {
            if (waiting) retry_attempts.push_back(info.retry_attempt);
        });

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 4u);
    expect_same_request(provider.calls[0], provider.calls[1]);
    EXPECT_EQ(provider.calls[0].size(), 3u);
    EXPECT_EQ(provider.calls[2].size(), 2u);
    expect_same_request(provider.calls[2], provider.calls[3]);
    EXPECT_EQ(result.compaction_request_retries, 2);
    EXPECT_EQ(result.compaction_request_items_removed, 1);
    EXPECT_EQ(retry_attempts, (std::vector<int>{1, 1}));
}

TEST(CompactCore, UsesCodexByteTokenEstimateAndUtf8SafeTruncation) {
    EXPECT_EQ(acecode::approx_token_count(""), 0u);
    EXPECT_EQ(acecode::approx_token_count("a"), 1u);
    EXPECT_EQ(acecode::approx_token_count("abcd"), 1u);
    EXPECT_EQ(acecode::approx_token_count("abcde"), 2u);

    const std::string chinese = u8"甲乙丙丁戊己庚辛壬癸";
    const std::string truncated =
        acecode::truncate_text_to_token_budget(chinese, 2);
    EXPECT_NE(truncated.find("tokens truncated"), std::string::npos);
    EXPECT_NO_THROW({
        nlohmann::json value = truncated;
        (void)value.dump();
    });
}

TEST(CompactCore, AutomaticThresholdsMatchCodexPercentages) {
    EXPECT_EQ(acecode::get_effective_context_window(100000), 95000);
    EXPECT_EQ(acecode::get_auto_compact_threshold(100000), 90000);
    EXPECT_FALSE(acecode::should_auto_compact(100000, 89999, 100));
    EXPECT_TRUE(acecode::should_auto_compact(100000, 90000, 100));
    EXPECT_TRUE(acecode::should_auto_compact(100000, 100, 90000));
}

TEST(CompactCore, ContextOverflowClassificationHandlesProviderShapes) {
    acecode::ProviderErrorInfo explicit_code;
    explicit_code.kind = acecode::ProviderErrorKind::Http;
    explicit_code.status_code = 400;
    explicit_code.raw_body =
        R"({"error":{"code":"context_length_exceeded"}})";
    EXPECT_TRUE(acecode::is_context_overflow_error(explicit_code));

    acecode::ProviderErrorInfo ambiguous_payload_too_large;
    ambiguous_payload_too_large.kind = acecode::ProviderErrorKind::Http;
    ambiguous_payload_too_large.status_code = 413;
    ambiguous_payload_too_large.raw_body =
        R"({"error":{"code":"payload_too_large","message":"upload too large"}})";
    EXPECT_FALSE(acecode::is_context_overflow_error(
        ambiguous_payload_too_large));

    acecode::ProviderErrorInfo strong_anthropic_shape;
    strong_anthropic_shape.kind = acecode::ProviderErrorKind::Http;
    strong_anthropic_shape.status_code = 400;
    strong_anthropic_shape.raw_body =
        R"({"type":"error","error":{"type":"invalid_request_error","message":"prompt is too long"}})";
    EXPECT_TRUE(acecode::is_context_overflow_error(strong_anthropic_shape));

    acecode::ProviderErrorInfo network;
    network.kind = acecode::ProviderErrorKind::Network;
    network.display_message = "connection reset";
    EXPECT_FALSE(acecode::is_context_overflow_error(network));

    acecode::ProviderErrorInfo retryable_timeout;
    retryable_timeout.kind = acecode::ProviderErrorKind::Timeout;
    retryable_timeout.retryable = true;
    EXPECT_TRUE(acecode::is_retryable_compaction_error(retryable_timeout));
}

// ---- 压缩摘要校验(fix-feedback-0924 第 3 条)--------------------------------

namespace {

// yubo2 会话第 1002 行 `[Conversation summary]` 的形态:压缩请求不带工具表,
// dots 模型接着历史「做下一步」,整段回复只有一个 dots 外壳包着的文本调用。
std::string yubo2_markup_only_summary() {
    return "\n\n<dots_function_call>\n"
           "<invoke name=\"Bash\">\n"
           "<parameter name=\"command\">\nGet-ChildItem -Recurse src\n</parameter>\n"
           "</invoke>\n"
           "</dots_function_call>";
}

std::string reminder_prompt() {
    return acecode::get_compact_prompt() + "\n\n" +
           acecode::get_compact_invalid_summary_reminder();
}

} // namespace

// 触发场景:模型把工具调用写成正文当摘要(yubo2 第 1002 行形态),第二次给出合法摘要。
// 期望行为:第一次被拒;第二次请求的提示词末尾带上「上次不是合法摘要」的提醒;
// 最终安装的是第二次的纯文本摘要。
// 回归:修复前 compact.cpp 原样收下 response.content,落盘的 `[Conversation summary]`
// 里只剩 `<dots_function_call>…`,之后模型越来越多地模仿文本调用。
TEST(CompactCore, ToolCallMarkupSummaryIsRetriedWithReminder) {
    ChatStubProvider provider;
    provider.responses.push_back(
        ChatStubProvider::response(yubo2_markup_only_summary()));
    provider.responses.push_back(
        ChatStubProvider::response("Progress: listed src; next run the tests."));

    auto result = acecode::compact_messages(
        provider, {msg("user", "inspect the repo", "u1")});

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 2u);
    EXPECT_EQ(provider.calls[0].back().content, acecode::get_compact_prompt());
    EXPECT_EQ(provider.calls[1].back().content, reminder_prompt());
    EXPECT_EQ(result.summary_text, "Progress: listed src; next run the tests.");
    EXPECT_EQ(result.compacted_messages.back().content,
              acecode::get_compact_summary_prefix() +
                  "\nProgress: listed src; next run the tests.");
}

// 触发场景:调用标记后面还跟着一段「下一步」正文。
// 期望行为:仍判为污染并重试 —— 标记出现在任意位置都算,不要求在末尾。
TEST(CompactCore, MarkupFollowedByProseIsStillRejected) {
    ChatStubProvider provider;
    provider.responses.push_back(ChatStubProvider::response(
        "<invoke name=\"Bash\">\n"
        "<parameter name=\"command\">ls</parameter>\n"
        "</invoke>\n\nNext steps: run the unit tests and fix failures."));
    provider.responses.push_back(ChatStubProvider::response("clean summary"));

    EXPECT_EQ(acecode::compact_summary_rejection_reason(
                  ChatStubProvider::response(
                      "Done so far.\n<invoke name=\"Bash\">\n</invoke>\nMore text.")),
              "tool_call_markup");

    auto result = acecode::compact_messages(provider, {msg("user", "request")});

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 2u);
    EXPECT_EQ(provider.calls[1].back().content, reminder_prompt());
    EXPECT_EQ(result.summary_text, "clean summary");
}

// 触发场景:模型在不带工具的压缩请求里回了原生 tool_calls,接着又回了只有空白的内容
// (DSML 标记被 provider 的 DSML 过滤器吞掉后就是这种空内容)。
// 期望行为:两次都被拒并重试,第三次的合法摘要被采用;原因分别是 tool_calls / empty。
TEST(CompactCore, NativeToolCallsOrBlankSummaryAreRetried) {
    auto with_tool_calls =
        ChatStubProvider::response("I will look at the files first.");
    with_tool_calls.tool_calls.push_back({"call-1", "bash", "{\"command\":\"ls\"}"});
    auto blank = ChatStubProvider::response(" \n\t\r\n ");

    EXPECT_EQ(acecode::compact_summary_rejection_reason(with_tool_calls),
              "tool_calls");
    EXPECT_EQ(acecode::compact_summary_rejection_reason(blank), "empty");
    EXPECT_EQ(acecode::compact_summary_rejection_reason(
                  ChatStubProvider::response("")),
              "empty");

    ChatStubProvider provider;
    provider.responses.push_back(with_tool_calls);
    provider.responses.push_back(blank);
    provider.responses.push_back(ChatStubProvider::response("final summary"));

    auto result = acecode::compact_messages(provider, {msg("user", "request")});

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 3u);
    EXPECT_EQ(provider.calls[1].back().content, reminder_prompt());
    EXPECT_EQ(provider.calls[2].back().content, reminder_prompt());
    EXPECT_EQ(result.summary_text, "final summary");
}

// 触发场景:模型连续 3 次都给出被污染的摘要。
// 期望行为:共请求 3 次(首次 + kMaxInvalidCompactSummaryRetries=2 次重试)后失败,
// 不安装任何摘要、不返回 compacted_messages;自动压缩由 AgentLoop 的丢弃最旧历史兜底接手。
// 上限 2 与 AgentLoop 的 kMaxEmptyResponseRetries 一致:两次仍不改就是在白烧 token。
TEST(CompactCore, InvalidSummaryExhaustsRetriesWithoutInstallingHistory) {
    ChatStubProvider provider;
    for (int i = 0; i < 4; ++i) {
        provider.responses.push_back(
            ChatStubProvider::response(yubo2_markup_only_summary()));
    }

    auto result = acecode::compact_messages(
        provider, {msg("user", "request")}, {}, true, nullptr);

    EXPECT_FALSE(result.performed);
    EXPECT_TRUE(result.compacted_messages.empty());
    EXPECT_TRUE(result.summary_text.empty());
    EXPECT_EQ(provider.calls.size(),
              static_cast<std::size_t>(
                  acecode::kMaxInvalidCompactSummaryRetries + 1));
    EXPECT_EQ(result.error,
              "Summarization returned an invalid summary (tool_call_markup) "
              "after 3 attempts.");
}

// 触发场景:摘要里在代码围栏和行内代码中引用了调用标签(例如记录「用户问过 XML 格式」)。
// 期望行为:一次通过 —— 围栏 / 行内代码里的内容是数据,不算污染。
TEST(CompactCore, FencedMarkupInsideSummaryIsAccepted) {
    const std::string summary =
        "The user asked how the XML tool format looks. Example shown:\n"
        "```xml\n"
        "<invoke name=\"Bash\">\n"
        "<parameter name=\"command\">ls</parameter>\n"
        "</invoke>\n"
        "```\n"
        "Inline mention: `<invoke name=\"Bash\">` is not supported.\n"
        "Next: run `cmake --build build`.";
    ChatStubProvider provider;
    provider.responses.push_back(ChatStubProvider::response(summary));

    auto result = acecode::compact_messages(provider, {msg("user", "request")});

    ASSERT_TRUE(result.performed) << result.error;
    EXPECT_EQ(provider.calls.size(), 1u);
    EXPECT_EQ(result.summary_text, summary);
}

// 触发场景:合法的中文短摘要,只有几个字。
// 期望行为:一次通过。故意不设长度下限:中文合法摘要可能只有几个字,
// 且大量现有用例的 stub 摘要就是 "summary" / "ok" 这样的短词。
TEST(CompactCore, ShortChineseSummaryIsAccepted) {
    const std::string summary = "\xE5\xB0\x9A\xE6\x97\xA0\xE8\xBF\x9B\xE5\xB1\x95\xE3\x80\x82";  // 尚无进展。
    ChatStubProvider provider;
    provider.responses.push_back(ChatStubProvider::response(summary));

    auto result = acecode::compact_messages(provider, {msg("user", "request")});

    ASSERT_TRUE(result.performed) << result.error;
    EXPECT_EQ(provider.calls.size(), 1u);
    EXPECT_EQ(result.summary_text, summary);
    EXPECT_EQ(acecode::compact_summary_rejection_reason(
                  ChatStubProvider::response(summary)),
              "");
}

// 触发场景:检查压缩提示词本身。
// 期望行为:保留 Codex 原文开头;末尾追加「不能调工具、不要输出调用标签、用纯文本」,
// 同时明确允许引用命令、路径和代码(原提示词要求保留关键数据与引用,不能被削弱)。
TEST(CompactCore, PromptForbidsToolCallsButAllowsQuotingCommands) {
    const std::string& prompt = acecode::get_compact_prompt();
    EXPECT_EQ(prompt.rfind("You are performing a CONTEXT CHECKPOINT COMPACTION.", 0), 0u);
    EXPECT_NE(prompt.find("Any critical data, examples, or references needed to continue"),
              std::string::npos);
    EXPECT_NE(prompt.find("tools are not available for this request"), std::string::npos);
    EXPECT_NE(prompt.find("do not call any tool"), std::string::npos);
    EXPECT_NE(prompt.find("do not emit tool-call or function-call tags"), std::string::npos);
    EXPECT_NE(prompt.find("You may still quote commands, paths and code"), std::string::npos);

    const std::string& reminder = acecode::get_compact_invalid_summary_reminder();
    EXPECT_NE(reminder.find("not a valid summary"), std::string::npos);
    EXPECT_NE(reminder.find("plain text"), std::string::npos);
}

// 触发场景:第一次摘要被污染,重试请求又撞上下文溢出,第三次成功。
// 期望行为:两套重试计数互不共享 —— 溢出照常丢一条最旧历史,而提醒在溢出重试的
// 请求里依然保留(说明「摘要不合格」的状态没有被溢出重试清掉)。
TEST(CompactCore, InvalidSummaryCounterIsIndependentOfOverflowRetries) {
    ChatStubProvider provider;
    provider.responses.push_back(
        ChatStubProvider::response(yubo2_markup_only_summary()));
    provider.responses.push_back(ChatStubProvider::response(
        "maximum context length exceeded", "error"));
    provider.responses.push_back(ChatStubProvider::response("summary"));
    std::vector<acecode::ChatMessage> messages{
        msg("user", "oldest"),
        msg("assistant", "middle"),
        msg("user", "newest"),
    };

    auto result = acecode::compact_messages(provider, messages);

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 3u);
    EXPECT_EQ(provider.calls[0].back().content, acecode::get_compact_prompt());
    EXPECT_EQ(provider.calls[1].back().content, reminder_prompt());
    EXPECT_EQ(provider.calls[2].back().content, reminder_prompt());
    EXPECT_EQ(provider.calls[2].size(), provider.calls[1].size() - 1);
    EXPECT_EQ(result.compaction_request_items_removed, 1);
    EXPECT_EQ(result.summary_text, "summary");
}
