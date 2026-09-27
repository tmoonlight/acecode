#include "compact.hpp"
#include "compact_prompt.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/session_history_recovery.hpp"
#include "pa/pa_quirks.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains_any(const std::string& haystack,
                  const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (haystack.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool is_utf8_continuation(unsigned char value) {
    return (value & 0xC0u) == 0x80u;
}

bool has_internal_user_context_metadata(const acecode::ChatMessage& msg) {
    if (!msg.metadata.is_object()) return false;

    static constexpr const char* kInternalContextKeys[] = {
        "transcript_only",
        "hidden_goal_context",
        "hidden_plan_mode_context",
        "hidden_todo_context",
        "hidden_hook_stop_continuation",
        "compact_initial_context",
    };
    for (const char* key : kInternalContextKeys) {
        if (msg.metadata.value(key, false)) return true;
    }
    return false;
}

std::string truncation_marker(std::size_t removed_tokens) {
    const char* ellipsis = "\xE2\x80\xA6";
    return std::string(ellipsis) + std::to_string(removed_tokens) +
           " tokens truncated" + ellipsis;
}

std::size_t message_payload_bytes(const acecode::ChatMessage& msg) {
    std::size_t bytes = msg.content.size() + msg.reasoning_content.size();
    if (msg.content_parts.is_array() && !msg.content_parts.empty()) {
        bytes += msg.content_parts.dump().size();
    }
    if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
        bytes += msg.tool_calls.dump().size();
    }
    bytes += msg.tool_call_id.size();
    // Account for the role and request envelope without pretending to have a
    // provider-specific tokenizer.
    bytes += msg.role.size() + 16;
    return bytes;
}

std::string provider_error_search_text(const acecode::ProviderErrorInfo& info) {
    return ascii_lower(info.display_message + "\n" + info.raw_body + "\n" +
                       info.pretty_json);
}

bool has_context_overflow_code(const nlohmann::json& value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if ((it.key() == "code" || it.key() == "type") &&
                it.value().is_string()) {
                const std::string marker = ascii_lower(it.value().get<std::string>());
                static const std::set<std::string> kCodes = {
                    "context_length_exceeded",
                    "context_window_exceeded",
                    "input_too_long",
                    "prompt_too_long",
                };
                if (kCodes.find(marker) != kCodes.end()) return true;
            }
            if (has_context_overflow_code(it.value())) return true;
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            if (has_context_overflow_code(item)) return true;
        }
    }
    return false;
}

bool raw_body_has_context_overflow_code(const std::string& body) {
    if (body.empty()) return false;
    try {
        return has_context_overflow_code(nlohmann::json::parse(body));
    } catch (const nlohmann::json::parse_error&) {
        return false;
    }
}

std::string compact_trigger_name(bool is_auto) {
    return is_auto ? "auto" : "manual";
}

int remove_oldest_history_item(
    std::vector<acecode::ChatMessage>& history) {
    if (history.empty()) return 0;

    acecode::ChatMessage removed = std::move(history.front());
    history.erase(history.begin());
    int removed_count = 1;

    if (removed.role == "assistant" && removed.tool_calls.is_array()) {
        std::set<std::string> call_ids;
        for (const auto& call : removed.tool_calls) {
            if (call.is_object() && call.contains("id") &&
                call["id"].is_string()) {
                call_ids.insert(call["id"].get<std::string>());
            }
        }
        const auto old_size = history.size();
        history.erase(
            std::remove_if(
                history.begin(), history.end(),
                [&](const acecode::ChatMessage& message) {
                    return message.role == "tool" &&
                           call_ids.find(message.tool_call_id) != call_ids.end();
                }),
            history.end());
        removed_count += static_cast<int>(old_size - history.size());
    } else if (removed.role == "tool" && !removed.tool_call_id.empty()) {
        for (auto it = history.begin(); it != history.end(); ++it) {
            if (it->role != "assistant" || !it->tool_calls.is_array()) continue;

            nlohmann::json retained_calls = nlohmann::json::array();
            bool found = false;
            for (const auto& call : it->tool_calls) {
                const bool matches =
                    call.is_object() && call.contains("id") &&
                    call["id"].is_string() &&
                    call["id"].get<std::string>() == removed.tool_call_id;
                if (matches) {
                    found = true;
                } else {
                    retained_calls.push_back(call);
                }
            }
            if (!found) continue;

            if (retained_calls.empty() && it->content.empty() &&
                it->reasoning_content.empty()) {
                history.erase(it);
                ++removed_count;
            } else {
                it->tool_calls = std::move(retained_calls);
            }
            break;
        }
    }
    return removed_count;
}

// 压缩请求被判上下文超限时收缩被总结的历史。前 kSingleItemOverflowRetries 次
// 每次只删最旧的一条(连同配对的 tool 结果),尽量多保留可总结的内容;之后每次
// 至少删掉约 1/4 的估算 token。起因(yubo2):换到窗口更小的模型后,旧历史比新
// 窗口多出几十万 token,一次删一条要来回请求几百次。
constexpr int kSingleItemOverflowRetries = 3;

int estimate_history_tokens(const std::vector<acecode::ChatMessage>& history) {
    std::size_t bytes = 0;
    for (const auto& msg : history) bytes += message_payload_bytes(msg);
    const std::size_t tokens = (bytes + 3) / 4;
    return tokens > static_cast<std::size_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(tokens);
}

int shrink_history_for_overflow(std::vector<acecode::ChatMessage>& history,
                                int overflow_retries) {
    if (history.empty()) return 0;
    if (overflow_retries <= kSingleItemOverflowRetries) {
        return remove_oldest_history_item(history);
    }
    const int before = estimate_history_tokens(history);
    const int target = before - before / 4;
    int removed = 0;
    do {
        removed += remove_oldest_history_item(history);
    } while (!history.empty() && estimate_history_tokens(history) > target);
    return removed;
}

} // namespace

namespace acecode {

std::size_t approx_token_count(const std::string& text) {
    constexpr std::size_t kApproxBytesPerToken = 4;
    const std::size_t padded =
        text.size() > std::numeric_limits<std::size_t>::max() -
                          (kApproxBytesPerToken - 1)
        ? std::numeric_limits<std::size_t>::max()
        : text.size() + kApproxBytesPerToken - 1;
    return padded / kApproxBytesPerToken;
}

std::string truncate_text_to_token_budget(const std::string& text,
                                          std::size_t max_tokens) {
    if (text.empty()) return {};

    constexpr std::size_t kApproxBytesPerToken = 4;
    const std::size_t max_bytes = max_tokens >
            std::numeric_limits<std::size_t>::max() / kApproxBytesPerToken
        ? std::numeric_limits<std::size_t>::max()
        : max_tokens * kApproxBytesPerToken;

    if (max_tokens > 0 && text.size() <= max_bytes) return text;
    if (max_bytes == 0) return truncation_marker(approx_token_count(text));

    const std::size_t left_budget = max_bytes / 2;
    const std::size_t right_budget = max_bytes - left_budget;

    std::size_t prefix_end = std::min(left_budget, text.size());
    while (prefix_end > 0 && prefix_end < text.size() &&
           is_utf8_continuation(static_cast<unsigned char>(text[prefix_end]))) {
        --prefix_end;
    }

    std::size_t suffix_start = text.size() > right_budget
        ? text.size() - right_budget
        : 0;
    while (suffix_start < text.size() &&
           is_utf8_continuation(static_cast<unsigned char>(text[suffix_start]))) {
        ++suffix_start;
    }
    if (suffix_start < prefix_end) suffix_start = prefix_end;

    const std::size_t removed_bytes = text.size() > max_bytes
        ? text.size() - max_bytes
        : 0;
    const std::size_t removed_tokens =
        (removed_bytes + kApproxBytesPerToken - 1) / kApproxBytesPerToken;
    return text.substr(0, prefix_end) + truncation_marker(removed_tokens) +
           text.substr(suffix_start);
}

int estimate_message_tokens(const std::vector<ChatMessage>& messages) {
    std::size_t total_bytes = 0;
    for (const auto& msg : messages) {
        const std::size_t bytes = message_payload_bytes(msg);
        if (total_bytes > std::numeric_limits<std::size_t>::max() - bytes) {
            return std::numeric_limits<int>::max();
        }
        total_bytes += bytes;
    }
    const std::size_t tokens = (total_bytes + 3) / 4;
    return tokens > static_cast<std::size_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(tokens);
}

bool is_compact_summary_message(const ChatMessage& msg) {
    const std::string prefix = get_compact_summary_prefix() + "\n";
    return msg.is_compact_summary || msg.content.rfind(prefix, 0) == 0;
}

bool is_real_user_message(const ChatMessage& msg) {
    if (msg.role != "user" || msg.is_meta) return false;
    if (has_internal_user_context_metadata(msg)) return false;
    return !is_compact_summary_message(msg);
}

std::vector<ChatMessage> build_compacted_history(
    const std::vector<ChatMessage>& messages,
    const std::string& summary_text,
    std::size_t max_user_message_tokens) {
    std::vector<ChatMessage> selected;
    std::size_t remaining = max_user_message_tokens;

    if (remaining > 0) {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (!is_real_user_message(*it)) continue;
            if (remaining == 0) break;

            ChatMessage retained = *it;
            const std::size_t tokens = approx_token_count(retained.content);
            if (tokens <= remaining) {
                remaining -= tokens;
            } else {
                retained.content =
                    truncate_text_to_token_budget(retained.content, remaining);
                remaining = 0;
            }

            retained.role = "user";
            // Structured parts are dropped (images, contexts), except `file`
            // parts: a file reference is a few hundred bytes, and dropping it
            // takes away the only read path the model has for that file. A
            // large paste stored as a file would otherwise vanish from the
            // model's input after every compaction. When parts are kept, the
            // (possibly truncated) text goes first as a text part, because
            // providers render content_parts instead of content.
            nlohmann::json retained_parts = nlohmann::json::array();
            if (it->content_parts.is_array()) {
                for (const auto& part : it->content_parts) {
                    if (part.is_object() &&
                        part.value("type", std::string{}) == "file") {
                        retained_parts.push_back(part);
                    }
                }
            }
            if (!retained_parts.empty() && !retained.content.empty()) {
                retained_parts.insert(retained_parts.begin(), nlohmann::json{
                    {"type", "text"}, {"text", retained.content}});
            }
            // 只有图片的用户消息:图片部件在上面被丢掉,content 又是空的,留下一条
            // 空内容的 user 消息 —— 部分服务端直接 400「message content cannot be
            // empty」(yubo2 反馈:压缩后切到 agnes-3.0-flash 每次请求都失败)。
            // 留一句占位说明;本来就什么都没有的消息直接跳过。
            if (retained_parts.empty() &&
                retained.content.find_first_not_of(" \t\r\n") == std::string::npos) {
                const bool had_parts =
                    it->content_parts.is_array() && !it->content_parts.empty();
                if (!had_parts) continue;
                retained.content = "[Image attachment omitted during context compaction]";
            }
            retained.content_parts = std::move(retained_parts);
            retained.tool_calls = nlohmann::json();
            retained.tool_call_id.clear();
            retained.reasoning_content.clear();
            retained.subtype.clear();
            retained.is_meta = false;
            retained.is_compact_summary = false;
            selected.push_back(std::move(retained));
        }
        std::reverse(selected.begin(), selected.end());
    }

    ChatMessage summary;
    summary.role = "user";
    summary.content = get_compact_user_summary_message(summary_text);
    summary.is_compact_summary = true;
    summary.metadata = nlohmann::json{{"compact_summary", true}};
    selected.push_back(std::move(summary));
    return selected;
}

std::vector<ChatMessage> normalize_messages_for_api(
    const std::vector<ChatMessage>& messages) {
    return recover_provider_history(provider_relevant_messages(messages)).messages;
}

int get_effective_context_window(int context_window) {
    if (context_window <= 0) return 0;
    const long long effective =
        static_cast<long long>(context_window) *
        EFFECTIVE_CONTEXT_WINDOW_PERCENT / 100;
    return effective > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(effective);
}

int get_auto_compact_threshold(int context_window) {
    if (context_window <= 0) return 0;
    const long long automatic =
        static_cast<long long>(context_window) *
        AUTO_COMPACT_CONTEXT_WINDOW_PERCENT / 100;
    return std::min(
        get_effective_context_window(context_window),
        automatic > std::numeric_limits<int>::max()
            ? std::numeric_limits<int>::max()
            : static_cast<int>(automatic));
}

bool should_auto_compact(int context_window,
    int server_total_tokens,
    int current_request_estimated_tokens) {
    const int threshold = get_auto_compact_threshold(context_window);
    if (context_window <= 0) return false;
    return std::max(server_total_tokens, current_request_estimated_tokens) >=
           threshold;
}

TokenWarningState calculate_token_warning_state(int estimated_tokens,
                                                 int context_window) {
    TokenWarningState state;
    const int effective = get_effective_context_window(context_window);
    if (effective <= 0) {
        state.percent_left = 0.0;
        state.is_above_warning = true;
        state.is_above_error = true;
        state.is_above_auto_compact = true;
        return state;
    }

    const int remaining = effective - estimated_tokens;
    state.percent_left =
        static_cast<double>(remaining) / static_cast<double>(effective) * 100.0;
    state.is_above_warning = remaining < 20000;
    state.is_above_error = remaining < 5000;
    state.is_above_auto_compact =
        estimated_tokens >= get_auto_compact_threshold(context_window);
    return state;
}

bool is_context_overflow_error(const std::string& error_message) {
    const std::string text = ascii_lower(error_message);
    static const std::vector<std::string> needles = {
        "context_length_exceeded",
        "context_window_exceeded",
        "maximum context length",
        "max context length",
        "context length exceeded",
        "context window exceeded",
        "exceeds the context window",
        "exceeded the context window",
        "prompt is too long",
        "input is too long",
        "too many input tokens",
        // vLLM / LiteLLM 转发的写法(yubo2 切到 agnes-2.5-flash 后的原文):
        // "ContextWindowExceededError: ... The input (564686 tokens) is longer than
        // the model's context length (524288 tokens)." 认不出时压缩内部「删最旧历史
        // 再重试」不启动,退化成机械裁剪,一次丢掉 527 条消息且没有摘要。
        "contextwindowexceeded",
        "longer than the model's context length",
        "exceeds the model's context length",
    };
    return contains_any(text, needles);
}

bool is_context_overflow_error(const ProviderErrorInfo& info) {
    // 客制化服务端的报文不走公开协议:文案可能是中文、type 可能是非标准值、
    // 状态码本身也不可信,下面这套 HTTP + 标准 code 的判定一条都不命中。
    // 认不出的后果不是多报一个错,而是 handle_provider_error 里那条三级恢复链
    // (修剪历史重试 → 精简请求档重试)整个不启动。判定收在 src/pa/。
    if (pa::is_context_overflow(info)) return true;
    if (info.kind != ProviderErrorKind::Http) return false;
    if (info.status_code != 400 && info.status_code != 413 &&
        info.status_code != 422) {
        return false;
    }
    return raw_body_has_context_overflow_code(info.raw_body) ||
           is_context_overflow_error(provider_error_search_text(info));
}

bool is_retryable_compaction_error(const ProviderErrorInfo& info) {
    return info.has_error() && info.retryable &&
           info.kind != ProviderErrorKind::UserCancelled &&
           !is_context_overflow_error(info);
}

void insert_context_before_last_real_user_or_summary(
    std::vector<ChatMessage>& messages,
    std::vector<ChatMessage> context) {
    if (context.empty()) return;

    auto fallback_summary = messages.end();
    auto insertion = messages.end();
    for (auto it = messages.end(); it != messages.begin();) {
        --it;
        if (it->role != "user") continue;
        if (is_compact_summary_message(*it)) {
            if (fallback_summary == messages.end()) fallback_summary = it;
            continue;
        }
        if (is_real_user_message(*it)) {
            insertion = it;
            break;
        }
    }
    if (insertion == messages.end()) insertion = fallback_summary;
    messages.insert(
        insertion,
        std::make_move_iterator(context.begin()),
        std::make_move_iterator(context.end()));
}

std::string compact_summary_rejection_reason(const ChatResponse& response) {
    // 压缩请求不带工具表,仍有模型(实测 dots3)接着历史里的 tool_calls「做下一步」:
    // 要么真的回原生 tool_calls,要么把调用写成正文。两种都不是摘要,绝不能落盘。
    if (response.has_tool_calls()) return "tool_calls";
    const bool blank = std::all_of(
        response.content.begin(), response.content.end(), [](char c) {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        });
    // DSML 标记会被 provider 的 DSML 过滤器吞掉,剩下的空内容在这里被拦住。
    if (blank) return "empty";
    // 任意位置出现调用标记都算污染(标记后面还跟着正文也一样);围栏与行内代码里的
    // 不算 —— 摘要引用命令、代码是允许的。刻意不设长度下限:中文合法摘要可能只有几个字。
    if (text_contains_tool_call_markup(response.content)) return "tool_call_markup";
    return {};
}

CompactResult compact_messages(
    LlmProvider& provider,
    const std::vector<ChatMessage>& messages,
    const std::vector<ChatMessage>& initial_context,
    bool is_auto,
    std::atomic<bool>* abort_flag,
    CompactRetryCallback on_retry) {
    CompactResult result;
    const std::vector<ChatMessage> original_history =
        normalize_messages_for_api(messages);
    std::vector<ChatMessage> request_history = original_history;
    // 被总结的模型同样不该看到旧的文本工具调用样本(与主请求同一套清洗),
    // 否则 dots 这类模型会接着把调用写进摘要(yubo2 现场)。
    sanitize_text_tool_call_history(request_history, get_compact_summary_prefix());

    if (provider.supports_native_compaction()) {
        LOG_WARN("Provider advertises native compaction but the active LlmProvider contract "
                 "did not return a validated native compaction item; using local compaction");
    }

    LOG_INFO("Compact start; trigger=" + compact_trigger_name(is_auto) +
             " history_items=" + std::to_string(original_history.size()) +
             " initial_context_items=" + std::to_string(initial_context.size()));

    std::string summary_suffix;
    std::uint64_t transient_retries = 0;
    // 与上下文溢出、瞬时错误的重试互不共享计数。
    int invalid_summary_retries = 0;
    int overflow_retries = 0;
    for (;;) {
        if (abort_flag && abort_flag->load()) {
            result.error = "Compaction cancelled.";
            return result;
        }

        std::vector<ChatMessage> request;
        request.reserve(initial_context.size() + request_history.size() + 1);
        auto stable_context = normalize_messages_for_api(initial_context);
        request.insert(request.end(), stable_context.begin(), stable_context.end());
        request.insert(request.end(), request_history.begin(), request_history.end());

        ChatMessage prompt;
        prompt.role = "user";
        prompt.content = invalid_summary_retries > 0
            ? get_compact_prompt() + "\n\n" +
                  get_compact_invalid_summary_reminder()
            : get_compact_prompt();
        request.push_back(std::move(prompt));

        try {
            LOG_INFO("Compact summarization call; trigger=" +
                     compact_trigger_name(is_auto) +
                     " request_items=" + std::to_string(request.size()) +
                     " removable_history_items=" +
                     std::to_string(request_history.size()) +
                     " removed_items=" +
                     std::to_string(result.compaction_request_items_removed));
            ChatResponse response = provider.chat(request, {});

            if (abort_flag && abort_flag->load()) {
                result.error = "Compaction cancelled.";
                return result;
            }

            if (response.finish_reason == "error") {
                const bool has_structured_error = response.provider_error.has_error();
                const bool context_overflow = has_structured_error
                    ? is_context_overflow_error(response.provider_error)
                    : is_context_overflow_error(response.content);
                if (context_overflow &&
                    !request_history.empty()) {
                    const int removed = shrink_history_for_overflow(
                        request_history, ++overflow_retries);
                    result.compaction_request_items_removed += removed;
                    transient_retries = 0;
                    LOG_WARN("Context window exceeded while compacting; removed " +
                             std::to_string(removed) + " oldest history item(s) (retry " +
                             std::to_string(overflow_retries) + ")");
                    continue;
                }

                if (has_structured_error &&
                    is_retryable_compaction_error(response.provider_error)) {
                    if (transient_retries !=
                        (std::numeric_limits<std::uint64_t>::max)()) {
                        ++transient_retries;
                    }
                    if (result.compaction_request_retries !=
                        (std::numeric_limits<int>::max)()) {
                        ++result.compaction_request_retries;
                    }
                    std::optional<std::int64_t> server_delay;
                    if (response.provider_error.server_retry_after_ms >= 0) {
                        server_delay =
                            response.provider_error.server_retry_after_ms;
                    }
                    const int delay_ms = static_cast<int>(
                        provider_retry_delay_ms(
                            transient_retries, server_delay));
                    ProviderErrorInfo retry_info = response.provider_error;
                    retry_info.retry_attempt =
                        saturating_retry_attempt(transient_retries);
                    retry_info.retry_max_attempts = -1;
                    retry_info.retry_delay_ms = delay_ms;
                    LOG_WARN("Retrying compaction request after transient provider error; "
                             "retry=" +
                             std::to_string(retry_info.retry_attempt) +
                             "/unbounded" +
                             " delay_ms=" + std::to_string(delay_ms));
                    if (on_retry) on_retry(retry_info, true);
                    if (provider.wait_for_retry(
                            std::chrono::milliseconds(delay_ms),
                            abort_flag)) {
                        result.error = "Compaction cancelled.";
                        return result;
                    }
                    if (on_retry) on_retry(retry_info, false);
                    continue;
                }

                const std::string provider_message =
                    has_structured_error &&
                            !response.provider_error.display_message.empty()
                        ? response.provider_error.display_message
                        : response.content;
                result.error = context_overflow
                    ? "Context window exceeded while compacting with no removable history item."
                    : "Summarization failed: " + provider_message;
                return result;
            }

            const std::string rejection =
                compact_summary_rejection_reason(response);
            if (!rejection.empty()) {
                LOG_WARN("Compact summary rejected; reason=" + rejection +
                         " attempt=" +
                         std::to_string(invalid_summary_retries + 1) + "/" +
                         std::to_string(kMaxInvalidCompactSummaryRetries + 1) +
                         " tool_calls=" +
                         std::to_string(response.tool_calls.size()) +
                         " excerpt=" +
                         truncate_utf8_prefix(response.content, 300));
                if (invalid_summary_retries < kMaxInvalidCompactSummaryRetries) {
                    ++invalid_summary_retries;
                    continue;
                }
                // 不安装任何摘要:自动压缩由 AgentLoop 现有的丢弃最旧历史兜底接手。
                result.error = "Summarization returned an invalid summary (" +
                               rejection + ") after " +
                               std::to_string(kMaxInvalidCompactSummaryRetries + 1) +
                               " attempts.";
                return result;
            }

            summary_suffix = response.content;
            break;
        } catch (const std::exception& error) {
            const std::string message = error.what();
            if (is_context_overflow_error(message) && !request_history.empty()) {
                const int removed = shrink_history_for_overflow(
                    request_history, ++overflow_retries);
                result.compaction_request_items_removed += removed;
                transient_retries = 0;
                LOG_WARN("Context window exceeded while compacting; removed " +
                         std::to_string(removed) + " oldest history item(s) (retry " +
                         std::to_string(overflow_retries) + ")");
                continue;
            }
            result.error = is_context_overflow_error(message)
                ? "Context window exceeded while compacting with no removable history item."
                : "Summarization failed: " + message;
            return result;
        }
    }

    result.compacted_messages = build_compacted_history(
        original_history, summary_suffix, COMPACT_USER_MESSAGE_MAX_TOKENS);
    const int before_tokens = estimate_message_tokens(original_history);
    const int after_tokens = estimate_message_tokens(result.compacted_messages);
    const int retained_user_messages =
        std::max(0, static_cast<int>(result.compacted_messages.size()) - 1);

    result.performed = true;
    result.messages_compressed = std::max(
        0, static_cast<int>(original_history.size()) - retained_user_messages);
    result.estimated_tokens_saved = std::max(0, before_tokens - after_tokens);
    result.summary_text = std::move(summary_suffix);

    LOG_INFO("Compact complete; trigger=" + compact_trigger_name(is_auto) +
             " history_items_before=" + std::to_string(original_history.size()) +
             " history_items_after=" +
             std::to_string(result.compacted_messages.size()) +
             " request_items_removed=" +
             std::to_string(result.compaction_request_items_removed) +
             " estimated_tokens_saved=" +
             std::to_string(result.estimated_tokens_saved));
    return result;
}

} // namespace acecode
