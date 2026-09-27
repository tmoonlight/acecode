#pragma once

#include "retry_policy.hpp"

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <cstddef>
#include <nlohmann/json.hpp>

namespace acecode {

struct ChatMessage {
    std::string role;    // "system", "user", "assistant", "tool"
    std::string content;
    nlohmann::json content_parts; // neutral structured text/image/file/context parts

    // For assistant messages with tool calls
    nlohmann::json tool_calls; // array of tool_call objects, empty if none

    // For tool result messages
    std::string tool_call_id;

    // Chain-of-thought emitted by reasoning-mode LLMs (DeepSeek thinking,
    // Qwen, Moonshot, OpenRouter, …). DeepSeek requires the previous turn's
    // reasoning_content to be echoed back on assistant messages — see
    // openspec/changes/support-deepseek-reasoning. Empty for non-reasoning
    // models; never set on user / system / tool messages.
    std::string reasoning_content;

    // Metadata fields for persisted/session-only records.
    std::string uuid;                    // unique identifier
    std::string subtype;                 // e.g. "compact_checkpoint"; empty for normal messages
    std::string timestamp;               // ISO 8601 timestamp
    bool is_meta = false;                // meta-message (boundary etc.), not sent to API
    bool is_compact_summary = false;     // marks this message as a compact summary
    nlohmann::json metadata;             // extended metadata (compact stats etc.)

    // Runtime-only compact preview for TUI rendering of tool_call rows. Not
    // serialized to session JSONL. When empty, the TUI falls back to the
    // legacy `[Tool: X] {JSON}` format.
    std::string display_override;
};

struct UserInput {
    std::string text;
    std::string display_text;
    nlohmann::json content_parts = nlohmann::json::array();
    nlohmann::json metadata = nlohmann::json::object();

    bool has_content_parts() const {
        return !content_parts.is_null() && content_parts.is_array() && !content_parts.empty();
    }

    bool empty() const {
        return text.empty() && !has_content_parts();
    }
};

struct ToolCall {
    std::string id;
    std::string function_name;
    std::string function_arguments; // raw JSON string
};

// ACECode-side estimate of the provider prompt's composition. Providers only
// report aggregate prompt_tokens, so these fields are proportionally
// reconciled to that authoritative total before they are exposed to clients.
struct ContextUsageBreakdown {
    int system_prompt = 0;
    int project_rules = 0;
    int skills = 0;
    int builtin_tools = 0;
    int mcp_tools = 0;
    int conversation = 0;
    int dynamic_context = 0;
    bool has_data = false;
};

struct TokenUsage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    int cache_read_tokens = 0;   // from prompt_tokens_details.cached_tokens
    int cache_write_tokens = 0;  // from prompt_tokens_details.cache_write_tokens
    int reasoning_tokens = 0;    // from completion_tokens_details.reasoning_tokens
    bool has_data = false; // true if server returned usage info
    ContextUsageBreakdown context_breakdown;
};

struct ToolDef {
    std::string name;
    std::string description;
    nlohmann::json parameters; // JSON Schema object
};

enum class ProviderErrorKind {
    None,
    UserCancelled,
    Timeout,
    Network,
    Http,
    MalformedSse,
    MalformedJson,
    Unknown,
};

struct ProviderErrorInfo {
    ProviderErrorKind kind = ProviderErrorKind::None;
    int status_code = 0;
    std::string provider;
    std::string model;
    std::string request_id;
    std::string display_message;
    std::string raw_body;
    bool body_is_json = false;
    std::string pretty_json;
    bool retryable = false;
    int retry_attempt = 0;
    int retry_max_attempts = 0;
    int retry_delay_ms = 0;
    // Raw upstream Retry-After guidance for non-streaming callers. -1 means
    // absent/invalid; zero is a valid immediate retry.
    std::int64_t server_retry_after_ms = -1;

    bool has_error() const { return kind != ProviderErrorKind::None; }
};

// 文本形式工具调用(模型把调用写进正文而不是走原生 tool_calls)的诊断。
// provider(OpenAiCompatProvider 的 chat / parse_sse_stream)产出,经
// ChatResponse::text_tool_calls 与 Done 事件上报;AgentLoop 据此决定是否
// 注入纠正提示重试。实现见 src/provider/text_tool_call_recovery.{hpp,cpp}。
struct TextToolCallDiagnostic {
    enum class Outcome {
        None,              // 没有文本调用(或全是原生调用的回显)
        Recovered,         // 执行级认出且校验通过,已转成原生 ToolCall
        Rejected,          // 认出了调用意图,但不执行(非法 / 截断 / 块前有正文 / 可疑级)
        IgnoredWithNative, // 已有原生调用,另有与之不一致的文本调用未执行
    };
    Outcome outcome = Outcome::None;
    // function_calls|dots_function_call|invoke|tool_call_json|tool_call_function|dsml
    std::string format;
    std::string error;   // 英文,可直接写进纠正提示
    // unknown_tool|bad_param|truncated|truncated_by_length|prose_prefix|malformed|parse_error
    std::string reason;
    std::vector<std::string> attempted_tools;    // 模型写的原名(已去掉回显)
    std::vector<std::string> unexecuted_detail;  // IgnoredWithNative:"bash(command)" 形式
    std::string raw_excerpt; // UTF-8 安全截断到 4096 字节,仅供诊断(日志 / metadata)
    // 可疑级:可见正文从这个字节偏移开始截掉(命中行的行首);npos = 不截。
    std::size_t visible_cut = std::string::npos;
    int recovered_count = 0;
};

struct ChatResponse {
    std::string content;               // text reply (empty if tool_calls present)
    nlohmann::json content_parts = nlohmann::json::array(); // optional structured output parts
    std::string reasoning_content;     // chain-of-thought (DeepSeek thinking etc.)
    std::vector<ToolCall> tool_calls;  // empty if pure text reply
    std::string finish_reason;         // "stop", "tool_calls", etc.
    TokenUsage usage;
    // Non-streaming calls preserve the same structured failure contract as
    // StreamEvent::provider_error. It is populated when finish_reason == "error".
    ProviderErrorInfo provider_error;
    // 文本形式工具调用的诊断(Outcome::None = 无)。
    TextToolCallDiagnostic text_tool_calls;

    bool has_tool_calls() const { return !tool_calls.empty(); }
};

// Streaming event types for chat_stream()
//   ReasoningDelta — chain-of-thought fragment from a reasoning-mode model.
//   ToolCallDelta — safe metadata while a streaming provider is still
//                   accumulating a tool call; tool_call.function_arguments is
//                   partial and should not be rendered raw to users.
//   Callbacks are free to ignore it; today the agent loop drops it silently
//   and a future TUI panel can subscribe.
//   Retry — provider is retrying a transient failure. Timeout retries may occur
//           after provisional stream output; consumers should discard partial
//           assistant/tool state before accepting output from the next attempt.
//   RetryResume — the retry wait ended and the next request attempt is starting.
enum class StreamEventType {
    Delta,
    ToolCall,
    ToolCallDelta,
    Done,
    Error,
    Usage,
    Retry,
    RetryResume,
    ReasoningDelta,
};

struct StreamEvent {
    StreamEventType type;
    std::string content;        // Delta: token fragment
    // ToolCall: complete call; ToolCallDelta: partial metadata —— id / name 已知即填,
    // function_arguments 为空(增量只报累计字节数,不携带 raw partial arguments)。
    ToolCall tool_call;
    int tool_index = -1;        // ToolCall/ToolCallDelta: index within current assistant turn
    std::size_t tool_call_argument_bytes = 0; // ToolCallDelta: accumulated argument bytes
    std::string error;          // Error: description
    ProviderErrorInfo provider_error; // Error/Retry: structured provider failure
    TokenUsage usage;           // Usage: token counts from server
    // Done: provider-native structured assistant blocks needed for a later
    // request. Anthropic uses this to preserve signed thinking blocks across
    // tool turns; providers that do not expose such blocks leave it empty.
    nlohmann::json content_parts = nlohmann::json::array();
    // Done: finish_reason as reported by the server ("stop"/"length"/"tool_calls"...).
    // Empty when the upstream never reported one — some OpenAI-compatible gateways
    // omit it entirely, so consumers must treat it as a best-effort signal.
    std::string finish_reason;
    // Done:文本形式工具调用的诊断(Outcome::None = 无)。
    TextToolCallDiagnostic text_tool_calls;
    // ToolCallDelta:true = provider 正在扣住一段疑似文本工具调用(tool_index
    // 为 -1,tool_call_argument_bytes = 已扣住字节数),只是进度提示,不代表
    // 模型已经开始输出原生调用。
    bool text_tool_call_hold = false;
};

using StreamCallback = std::function<void(const StreamEvent&)>;

class LlmProvider {
public:
    virtual ~LlmProvider() = default;

    virtual ChatResponse chat(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools
    ) = 0;

    // Streaming chat: invokes callback for each event. abort_flag can cancel the request.
    virtual void chat_stream(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag = nullptr
    ) = 0;

    virtual std::string name() const = 0;
    virtual bool is_authenticated() = 0;
    // Native agent runtimes may own tools even when ACECode passes an empty
    // tool list. Detached read-only side chat must reject those runtimes.
    virtual bool supports_tool_free_chat() const { return true; }

    virtual std::string model() const = 0;
    virtual void set_model(const std::string& m) = 0;

    bool wait_for_retry(std::chrono::milliseconds delay,
                        const std::atomic<bool>* abort_flag) {
        return retry_waiter_.wait_for(delay, abort_flag);
    }
    void wake_retry_waiter() { retry_waiter_.wake(); }
    void notify_cancelled_request() { retry_waiter_.notify_cancelled_request(); }

    // Native Responses-style compaction requires provider-specific trigger and
    // response-item support. Chat providers remain on local compaction. A
    // future provider must override this only together with that full native
    // protocol; advertising support alone never fabricates native items.
    virtual bool supports_native_compaction() const { return false; }

    // 当前 active 模型能否直接读取图片附件。system prompt 的 # Environment 段
    // 与 vision_analyze 的自调用防护都读这一位:模型无法自我判定"我看得见图
    // 吗",不把这个事实显式喂给它,它就会在自己已经能看图时仍绕道去调
    // vision_analyze(实测会话 20260830-024351-9599:主模型 Aurora-aurora 带
    // vision 标签,却先 skill_view 再 vision_analyze,子调用挑中的还是它自己)。
    // 默认 true 是 fail-open,与 OpenAICompatProvider::model_has_vision_ 的默认
    // 口径一致 —— 未接线的 provider 维持旧行为(照发图片、不额外限制)。
    virtual bool supports_vision() const { return true; }

    virtual bool authenticate() { return true; }

private:
    ProviderRetryWaiter retry_waiter_;
};

} // namespace acecode
