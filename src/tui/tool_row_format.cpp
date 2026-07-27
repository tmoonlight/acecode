#include "tui/tool_row_format.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <utility>

namespace acecode { namespace tui {

namespace {

constexpr const char* kLegacyPrefix = "[Tool: ";

std::string trim_ascii_whitespace(std::string value) {
    auto is_space = [](unsigned char c) {
        return std::isspace(c) != 0;
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
        [is_space](unsigned char c) { return !is_space(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
        [is_space](unsigned char c) { return !is_space(c); }).base(),
        value.end());
    return value;
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return value;
}

bool has_non_whitespace(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) == 0;
    });
}

} // namespace

ToolRowParts parse_tool_row(const std::string& content,
                            const std::string& display_override) {
    ToolRowParts parts;

    std::string legacy_args;
    if (content.rfind(kLegacyPrefix, 0) == 0) {
        const size_t name_begin = std::char_traits<char>::length(kLegacyPrefix);
        const size_t name_end = content.find(']', name_begin);
        if (name_end != std::string::npos) {
            parts.name = content.substr(name_begin, name_end - name_begin);
            size_t args_begin = name_end + 1;
            if (args_begin < content.size() && content[args_begin] == ' ') {
                ++args_begin;
            }
            legacy_args = content.substr(args_begin);
        }
    }

    if (!display_override.empty()) {
        // label 与参数之间恒为两个空格且 label 不含双空格,取第一处即分隔符。
        // 没有双空格说明预览只有 label(如 wait_subagent),参数留空。
        const size_t sep = display_override.find("  ");
        if (sep != std::string::npos) {
            parts.args = display_override.substr(sep + 2);
        }
        // display_override 存在但 content 解析失败时(不应发生),用 label
        // 兜底当工具名,保证行不至于整行降级。
        if (parts.name.empty()) {
            parts.name = sep == std::string::npos
                ? display_override
                : display_override.substr(0, sep);
        }
    } else {
        parts.args = legacy_args;
    }

    // 空参数 JSON "{}" 视为无参数,渲染层就不用画空括号了。
    if (parts.args == "{}") {
        parts.args.clear();
    }
    return parts;
}

std::string pascal_case_tool_name(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    bool upper_next = true;
    for (char c : name) {
        if (c == '_') {
            upper_next = true;
            continue;
        }
        if (upper_next && c >= 'a' && c <= 'z') {
            out.push_back(static_cast<char>(c - 'a' + 'A'));
        } else {
            out.push_back(c);
        }
        upper_next = false;
    }
    return out;
}

bool tool_result_row_failed(const TuiState::Message& msg) {
    if (msg.content.rfind("[Error]", 0) == 0 ||
        msg.content.rfind("[User denied tool execution]", 0) == 0) {
        return true;
    }
    if (msg.summary.has_value()) {
        for (const auto& kv : msg.summary->metrics) {
            if (kv.first == "exit" && kv.second != "0") return true;
            if (kv.first == "aborted" && kv.second == "true") return true;
            if (kv.first == "timeout" && kv.second == "true") return true;
        }
    }
    return false;
}

bool ToolRowMetadataWindow::contains(
    std::size_t conversation_index) const noexcept {
    return conversation_index >= first_message &&
        conversation_index < last_message_exclusive;
}

ToolCallDot ToolRowMetadataWindow::call_dot_at(
    std::size_t conversation_index) const noexcept {
    if (!contains(conversation_index)) return ToolCallDot::Pending;
    const std::size_t offset = conversation_index - first_message;
    return offset < call_dots.size()
        ? call_dots[offset]
        : ToolCallDot::Pending;
}

const std::string& ToolRowMetadataWindow::result_name_at(
    std::size_t conversation_index) const noexcept {
    static const std::string kEmptyName;
    if (!contains(conversation_index)) return kEmptyName;
    const std::size_t offset = conversation_index - first_message;
    return offset < result_names.size()
        ? result_names[offset]
        : kEmptyName;
}

ToolRowMetadataWindow compute_tool_row_metadata_window(
    const std::vector<TuiState::Message>& conversation,
    std::size_t requested_first,
    std::size_t requested_last_exclusive) {
    const std::size_t conversation_size = conversation.size();
    std::size_t first = std::min(requested_first, conversation_size);
    std::size_t last = std::min(
        std::max(requested_last_exclusive, first),
        conversation_size);

    const auto is_tool_pair_role = [&conversation](std::size_t index) {
        const auto& role = conversation[index].role;
        return role == "tool_call" || role == "tool_result";
    };
    if (first < last && is_tool_pair_role(first)) {
        while (first > 0 && is_tool_pair_role(first - 1)) {
            --first;
        }
    }
    if (first < last && is_tool_pair_role(last - 1)) {
        while (last < conversation_size && is_tool_pair_role(last)) {
            ++last;
        }
    }

    ToolRowMetadataWindow metadata;
    metadata.first_message = first;
    metadata.last_message_exclusive = last;
    const std::size_t window_size = last - first;
    metadata.call_dots.assign(window_size, ToolCallDot::Pending);
    metadata.result_names.resize(window_size);

    struct PendingToolCall {
        std::size_t window_offset = 0;
        std::string name;
    };
    std::deque<PendingToolCall> unmatched_calls;
    for (std::size_t i = first; i < last; ++i) {
        const auto& msg = conversation[i];
        const std::size_t window_offset = i - first;
        if (msg.role == "tool_call") {
            unmatched_calls.push_back({
                window_offset,
                parse_tool_row(msg.content, msg.display_override).name,
            });
        } else if (msg.role == "tool_result") {
            if (!unmatched_calls.empty()) {
                auto pending = std::move(unmatched_calls.front());
                unmatched_calls.pop_front();
                metadata.call_dots[pending.window_offset] =
                    tool_result_row_failed(msg)
                    ? ToolCallDot::Failed
                    : ToolCallDot::Ok;
                metadata.result_names[window_offset] =
                    std::move(pending.name);
            }
        } else {
            // 批次边界:其他角色出现说明这一轮工具阶段已经结束,残留的
            // 未配对调用(abort / provider 中断)永远等不到结果,保持
            // Pending 并且绝不能吃掉后续轮次的结果。
            unmatched_calls.clear();
        }
    }
    return metadata;
}

std::vector<ToolCallDot> compute_tool_call_dots(
    const std::vector<TuiState::Message>& conversation) {
    auto metadata = compute_tool_row_metadata_window(
        conversation, 0, conversation.size());
    return std::move(metadata.call_dots);
}

std::vector<std::string> compute_tool_result_names(
    const std::vector<TuiState::Message>& conversation) {
    auto metadata = compute_tool_row_metadata_window(
        conversation, 0, conversation.size());
    return std::move(metadata.result_names);
}

bool is_task_complete_result(const TuiState::Message& msg,
                             const std::string& paired_tool_name) {
    if (msg.role != "tool_result") return false;

    const std::string normalized_name =
        ascii_lower(trim_ascii_whitespace(paired_tool_name));
    if (normalized_name == "task_complete" || normalized_name == "complete") {
        return true;
    }

    if (!msg.summary.has_value()) return false;
    const auto& summary = *msg.summary;
    const std::string verb =
        ascii_lower(trim_ascii_whitespace(summary.verb));
    const std::string object =
        ascii_lower(trim_ascii_whitespace(summary.object));
    if (verb != "complete") return false;
    if (object == "task") return true;
    return std::any_of(summary.metrics.begin(), summary.metrics.end(),
        [](const auto& metric) {
            return ascii_lower(trim_ascii_whitespace(metric.first)) ==
                "summary";
        });
}

std::string task_complete_summary_markdown(const TuiState::Message& msg) {
    if (msg.summary.has_value()) {
        for (const auto& metric : msg.summary->metrics) {
            if (ascii_lower(trim_ascii_whitespace(metric.first)) == "summary" &&
                has_non_whitespace(metric.second)) {
                return metric.second;
            }
        }

        const auto& object = msg.summary->object;
        if (has_non_whitespace(object) &&
            ascii_lower(trim_ascii_whitespace(object)) != "task") {
            return object;
        }
    }

    if (has_non_whitespace(msg.content)) return msg.content;
    return "Completed";
}

}} // namespace acecode::tui
