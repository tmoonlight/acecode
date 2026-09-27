#pragma once

// 工具执行结果的契约类型(P2-02 自 tool/tool_executor.hpp 拆出):session 的元数据编解码、
// 工具结果落盘与 web 事件序列化只需要这两个结构体,不必依赖整个 ToolExecutor。

#include "utils/diff_utils.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace acecode {

// Structured summary used by tool-result UIs to render a single-line row
// (icon + verb + object + dot-separated metrics). Tool implementations may
// provide a domain-specific summary; the execution boundary supplies a generic
// summary when they do not.
struct ToolSummary {
    std::string verb;     // "Ran" / "Read" / "Wrote" / "Created" / "Edited" ...
    std::string object;   // file path or command preview
    std::vector<std::pair<std::string, std::string>> metrics; // ordered
    std::string icon;     // short glyph (may be Unicode or ASCII fallback)
};

// Result of a tool execution
struct ToolResult {
    std::string output;
    bool success = true;
    std::optional<ToolSummary> summary; // always populated at the execution boundary
    // Optional UI/persistence metadata. This is never part of provider-visible
    // text; AgentLoop stores it on the ChatMessage and web lifecycle payloads.
    nlohmann::json metadata = nlohmann::json::object();
    // Optional user-role prompt to append after this tool result. This is used
    // for progressive capability disclosure that should affect only the active
    // conversation after a tool is explicitly opened, rather than the global
    // cacheable system prompt.
    std::optional<std::string> post_user_prompt;
    std::string post_user_prompt_display_text;
    // 结构化 diff hunk。file_edit / file_write 在产生 unified diff 文本的同时
    // 填充这个字段;TUI 用它做彩色带行号 gutter 的渲染。
    // 运行时字段 —— 不写入 session JSONL(由 session_serializer 的 allowlist
    // 天然挡住;新加字段时如果不加进白名单就不会被序列化)。
    std::optional<std::vector<DiffHunk>> hunks;
    // Structured output attachments produced by a tool. Items are either stored
    // AttachmentRecord JSON objects or pre-materialization descriptors such as
    // {name,mime_type,data_url} / {name,mime_type,path}. AgentLoop materializes
    // descriptors into session attachments before events and JSONL persistence.
    nlohmann::json attachments = nlohmann::json::array();
    std::vector<std::string> attachment_warnings;

    // Runtime-only terminal control. A tool that permanently removes the
    // calling session uses this to stop the current model turn after its
    // canonical tool result and terminal lifecycle events have been persisted.
    // Neither field is serialized into the provider-visible result.
    bool terminate_session_after_turn = false;
    std::function<void()> post_turn_action;

    bool has_attachments() const {
        return attachments.is_array() && !attachments.empty();
    }
};

} // namespace acecode
