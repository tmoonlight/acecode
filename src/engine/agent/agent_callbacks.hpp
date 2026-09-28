#pragma once


#include <nlohmann/json_fwd.hpp>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace acecode {

struct ChatMessage;
struct ToolResult;
struct TokenUsage;
struct ProviderErrorInfo;
enum class PermissionResult;
struct CompactResult;

// Callbacks for the TUI to observe agent loop events
struct AgentCallbacks {
    // Called when a new message is added to the conversation
    std::function<void(const std::string& role, const std::string& content, bool is_tool)> on_message;

    // Metadata-preserving observer for persisted transcript-only messages.
    // When installed, it receives those messages instead of the legacy
    // three-field on_message callback so UI grouping can use stable metadata.
    std::function<void(const ChatMessage& message)> on_transcript_message;

    // Called after each tool execution with the structured ToolResult so the
    // TUI can render a summary row. Fires in addition to on_message (not in
    // place of it) so consumers that only care about the text stream continue
    // to work unchanged. Receives the tool_call message too so the TUI can
    // correlate summaries with their call rows.
    std::function<void(const ChatMessage& call_msg,
                       const std::string& tool_name,
                       const ToolResult& result)> on_tool_result;

    // Called when the agent starts/stops processing
    std::function<void(bool busy)> on_busy_changed;

    // Called once for a submitted agent turn immediately before its terminal
    // busy=false callback. Values match persisted turn timing status:
    // "completed", "error", or "aborted". Compact/background busy cycles do
    // not invoke this hook.
    std::function<void(const std::string& status)> on_turn_finished;

    // Called to request user confirmation for a tool call.
    // Returns: Allow, Deny, or AlwaysAllow
    std::function<PermissionResult(const std::string& tool_name, const std::string& arguments)> on_tool_confirm;

    // Called for each streaming delta token (real-time TUI update)
    std::function<void(const std::string& token)> on_delta;

    // Called when token usage data is received from the provider
    std::function<void(const TokenUsage& usage)> on_usage;

    // Called when the current thread goal status changes. Empty string means
    // no goal is active for the current session.
    std::function<void(const std::string& status)> on_goal_status;

    // Called when TodoWrite publishes or reads the current visible checklist.
    // The payload shape matches the todo_updated session event.
    std::function<void(const nlohmann::json& payload)> on_todo_updated;

    // 具体进度提示(add-tool-preamble,「适合日常工作」):loading 文案变化时回调
    // (「正在分析你的请求」「正在读取 3 个文件」「正在分析命令输出」、推理加粗
    // 标题…),TUI 用它替换等待动画里的随机短语。关闭时不回调。
    std::function<void(const std::string& title)> on_thinking_title;

    // Legacy display observer for replacement-style transcript updates. Normal
    // compact success appends marker messages and no longer calls this hook.
    std::function<void(const std::vector<ChatMessage>& messages,
                       const CompactResult& result)> on_transcript_replace;

    // Called before a provider retry replays the current model request.
    // Consumers should clear provisional live assistant output from the failed
    // stream attempt; persisted history is unchanged.
    std::function<void()> on_stream_retry_reset;

    // Presentation-only retry lifecycle; neither callback appends transcript
    // messages.
    std::function<void(const ProviderErrorInfo&)> on_model_retry;
    std::function<void()> on_model_retry_resume;

    // Called just before a tool begins executing. `command_preview` is a short
    // human-readable summary (e.g. the first 60 chars of a bash command).
    // `preamble` 保留给以后用,当前恒为空:进度头是工具行,参数照常显示;具体
    // 进度提示只走 on_thinking_title(loading 行)。
    std::function<void(const std::string& tool_name,
                       const std::string& command_preview,
                       const std::string& preamble)> on_tool_progress_start;

    // Called from the tool's streaming thread with each cleaned chunk.
    // `tail_snapshot` is the last-5-lines sliding window; `current_partial` is
    // the in-progress line (not yet terminated by \n).
    std::function<void(const std::vector<std::string>& tail_snapshot,
                       const std::string& current_partial,
                       size_t total_bytes,
                       int total_lines)> on_tool_progress_update;

    // Called after the tool returns (or throws). Guaranteed via RAII to fire
    // once for every on_tool_progress_start.
    std::function<void()> on_tool_progress_end;
};

} // namespace acecode
