#include "tool_call_corruption.hpp"

#include <nlohmann/json.hpp>

namespace acecode {

namespace {

constexpr std::string_view kToolCallOpen = "<tool_call>";
constexpr std::string_view kToolCallClose = "</tool_call>";
constexpr std::string_view kArgKeyOpen = "<arg_key>";
constexpr std::string_view kArgKeyClose = "</arg_key>";
constexpr std::string_view kArgValueOpen = "<arg_value>";
constexpr std::string_view kArgValueClose = "</arg_value>";
constexpr std::string_view kGlmMarkers[] = {
    kToolCallOpen, kToolCallClose, kArgKeyOpen, kArgKeyClose, kArgValueOpen, kArgValueClose};

// 名字里出现这些字符,说明模型把调用表达式当成了名字:
// skill_view(name="x")、skill_view["x"]、{"name": …}。
constexpr std::string_view kCallSyntaxChars = "()[]{}\"'=<>`";

constexpr std::string_view kWhitespace = " \t\r\n";

bool is_blank(std::string_view text) {
    return text.find_first_not_of(kWhitespace) == std::string_view::npos;
}

// pos 起跳过空白后是否以 expected 开头。
bool followed_by(std::string_view text, std::size_t pos, std::string_view expected) {
    const auto next = text.find_first_not_of(kWhitespace, pos);
    return next != std::string_view::npos && text.compare(next, expected.size(), expected) == 0;
}

// 字符串值里的模板结构,命中返回结构开头的标记。规则见头文件。
std::optional<std::string> find_template_structure(std::string_view value) {
    for (auto pos = value.find(kArgKeyClose); pos != std::string_view::npos;
         pos = value.find(kArgKeyClose, pos + 1)) {
        if (followed_by(value, pos + kArgKeyClose.size(), kArgValueOpen)) {
            return std::string(kArgKeyClose);
        }
    }
    for (auto pos = value.find(kArgValueClose); pos != std::string_view::npos;
         pos = value.find(kArgValueClose, pos + 1)) {
        const std::size_t after = pos + kArgValueClose.size();
        if (is_blank(value.substr(after)) || followed_by(value, after, kArgKeyOpen) ||
            followed_by(value, after, kToolCallOpen) || followed_by(value, after, kToolCallClose)) {
            return std::string(kArgValueClose);
        }
    }
    return std::nullopt;
}

std::optional<std::string> find_corrupted_json_text(const nlohmann::json& value) {
    if (value.is_string()) return find_template_structure(value.get_ref<const std::string&>());
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (auto marker = find_glm_tool_call_marker(it.key())) return marker;
            if (auto marker = find_corrupted_json_text(it.value())) return marker;
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            if (auto marker = find_corrupted_json_text(item)) return marker;
        }
    }
    return std::nullopt;
}

} // namespace

bool is_conventional_tool_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (const char c : name) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

std::optional<std::string> find_glm_tool_call_marker(std::string_view text) {
    std::size_t earliest = std::string_view::npos;
    std::string_view found;
    for (const auto marker : kGlmMarkers) {
        const auto pos = text.find(marker);
        if (pos < earliest) {
            earliest = pos;
            found = marker;
        }
    }
    if (earliest == std::string_view::npos) return std::nullopt;
    return std::string(found);
}

std::optional<ToolCallCorruption> diagnose_corrupted_tool_name(std::string_view name) {
    using Field = ToolCallCorruption::Field;
    using Reason = ToolCallCorruption::Reason;
    if (is_blank(name)) return ToolCallCorruption{Field::Name, Reason::EmptyName, {}};
    if (is_conventional_tool_name(name)) return std::nullopt;
    if (auto marker = find_glm_tool_call_marker(name)) {
        return ToolCallCorruption{Field::Name, Reason::TemplateMarkup, std::move(*marker)};
    }
    const auto syntax = name.find_first_of(kCallSyntaxChars);
    if (syntax != std::string_view::npos) {
        return ToolCallCorruption{Field::Name, Reason::CallSyntax, std::string(1, name[syntax])};
    }
    return std::nullopt;
}

std::optional<ToolCallCorruption> diagnose_corrupted_tool_arguments(std::string_view arguments) {
    // 快路径:绝大多数参数里连标签名都没有(file_write 的参数可能有几百 KB)。
    if (arguments.find("arg_") == std::string_view::npos &&
        arguments.find("tool_call>") == std::string_view::npos) {
        return std::nullopt;
    }
    const auto parsed = nlohmann::json::parse(arguments, nullptr, false);
    std::optional<std::string> marker = parsed.is_discarded()
        ? find_glm_tool_call_marker(arguments)
        : find_corrupted_json_text(parsed);
    if (!marker) return std::nullopt;
    return ToolCallCorruption{ToolCallCorruption::Field::Arguments,
                              ToolCallCorruption::Reason::TemplateMarkup, std::move(*marker)};
}

std::optional<CorruptedToolCall> find_corrupted_tool_call(
    const std::vector<ToolCall>& calls,
    const std::function<bool(const std::string&)>& is_registered) {
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const auto& call = calls[i];
        if (!is_registered || !is_registered(call.function_name)) {
            if (auto corruption = diagnose_corrupted_tool_name(call.function_name)) {
                return CorruptedToolCall{i, std::move(*corruption)};
            }
        }
        if (auto corruption = diagnose_corrupted_tool_arguments(call.function_arguments)) {
            return CorruptedToolCall{i, std::move(*corruption)};
        }
    }
    return std::nullopt;
}

} // namespace acecode
