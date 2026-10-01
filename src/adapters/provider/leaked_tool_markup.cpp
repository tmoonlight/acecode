// 回复正文里泄漏的工具参数模板标记(<arg_key> / <arg_value>)检测,声明见
// text_tool_call_recovery.hpp。单独成文件让 text_tool_call_recovery.cpp 保持在
// 行数基线内。
#include "text_tool_call_recovery.hpp"

#include "markdown_fence_tracker.hpp"

namespace acecode {

std::optional<std::string> find_leaked_tool_argument_markup(std::string_view text) {
    static constexpr std::string_view kMarkers[] = {
        "<arg_key>", "</arg_key>", "<arg_value>", "</arg_value>"};
    // 快路径:绝大多数回复里连 "arg_" 都没有,不必逐字符跟踪围栏。
    if (text.find("arg_") == std::string_view::npos) return std::nullopt;
    MarkdownFenceTracker fence;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '<' && !fence.in_fence() && !fence.line_opening_fence() &&
            !fence.in_inline_code()) {
            for (const auto marker : kMarkers) {
                if (text.compare(i, marker.size(), marker) == 0) return std::string(marker);
            }
        }
        fence.feed(c);
    }
    return std::nullopt;
}

} // namespace acecode
