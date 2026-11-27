#include "auto_review_transcript.hpp"

#include "llm/message_predicates.hpp"

#include <map>
#include <utility>

namespace acecode::auto_review {
namespace {

constexpr const char* kUserDeniedPrefix = "[User denied tool execution]";
constexpr const char* kAskToolName = "AskUserQuestion";

bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// 取前 n 字节,落在字符边界上。
std::string utf8_head(const std::string& text, std::size_t n) {
    if (text.size() <= n) return text;
    std::size_t cut = n;
    while (cut > 0 && is_continuation(static_cast<unsigned char>(text[cut]))) --cut;
    return text.substr(0, cut);
}

// 取后 n 字节,落在字符边界上。
std::string utf8_tail(const std::string& text, std::size_t n) {
    if (text.size() <= n) return text;
    std::size_t start = text.size() - n;
    while (start < text.size() && is_continuation(static_cast<unsigned char>(text[start]))) ++start;
    return text.substr(start);
}

// 超长文本保留头尾:危险的部分既可能在开头(命令本身),也可能在末尾
// (`&& git push --force`),只留开头会让尾巴上的东西躲过检查。
std::string clip_middle(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) return text;
    const std::size_t head = limit * 2 / 3;
    const std::size_t tail = limit - head;
    const std::string first = utf8_head(text, head);
    const std::string last = utf8_tail(text, tail);
    const std::size_t omitted = text.size() - first.size() - last.size();
    return first + " ...[" + std::to_string(omitted) + " bytes omitted]... " + last;
}

char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool starts_with_ci(const std::string& text, std::size_t pos, const char* needle) {
    for (std::size_t i = 0; needle[i] != '\0'; ++i) {
        if (pos + i >= text.size() || lower_ascii(text[pos + i]) != needle[i]) return false;
    }
    return true;
}

// 记录外面包着 <transcript> / <action> 两个标签。用户粘贴的内容或工具参数里
// 如果自带这些标签,就能提前「闭合」记录、伪造一段待裁决的调用,所以把它们的
// 尖括号换成实体。只动这四个标签,其余文本原样保留。
std::string neutralize_tags(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '<' &&
            (starts_with_ci(text, i, "<transcript") || starts_with_ci(text, i, "</transcript") ||
             starts_with_ci(text, i, "<action") || starts_with_ci(text, i, "</action"))) {
            out += "&lt;";
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

// 一条记录占一行,正文里的换行变成缩进的续行。这样正文中间出现的 "User:" 或
// "Tool bash ..." 不会被读成新的一条 —— 行首不缩进的才是程序写的。
std::string entry(const std::string& label, const std::string& body) {
    std::string out = label;
    const std::string safe = neutralize_tags(body);
    for (const char c : safe) {
        if (c == '\r') continue;
        if (c == '\n') {
            out += "\n  | ";
            continue;
        }
        out.push_back(c);
    }
    out.push_back('\n');
    return out;
}

void shrink_strings(nlohmann::json& value, std::size_t limit) {
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() > limit) value = clip_middle(text, limit);
    } else if (value.is_object() || value.is_array()) {
        for (auto& child : value) shrink_strings(child, limit);
    }
}

std::string project_arguments(const std::string& arguments, std::size_t field_limit,
                              std::size_t total_limit) {
    auto parsed = nlohmann::json::parse(arguments, nullptr, false);
    std::string text;
    if (parsed.is_discarded()) {
        text = arguments;   // 不是合法 JSON:当普通文本处理。
    } else {
        shrink_strings(parsed, field_limit);
        text = parsed.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    return clip_middle(text, total_limit);
}

std::string strip_summary_prefix(const std::string& content) {
    const std::string& prefix = get_compact_summary_prefix();
    std::size_t start = content.rfind(prefix, 0) == 0 ? prefix.size() : 0;
    while (start < content.size() && (content[start] == '\n' || content[start] == ' ')) ++start;
    return content.substr(start);
}

bool metadata_flag(const ChatMessage& message, const char* key) {
    if (!message.metadata.is_object()) return false;
    const auto it = message.metadata.find(key);
    return it != message.metadata.end() && it->is_boolean() && it->get<bool>();
}

// 带网页批注的消息。content 里展开了网页标题、地址和被批注元素的原文,那些是
// 网页给的数据,网页可以随便写。用户的话只有两处:输入框里打的字(display_text)
// 和每条批注的描述(用户在批注框里写的)。两处都空时返回空串,绝不退回 content。
std::string annotated_user_text(const ChatMessage& message) {
    std::string text;
    const auto display = message.metadata.find("display_text");
    if (display != message.metadata.end() && display->is_string()) text = display->get<std::string>();
    const auto contexts = message.metadata.find("contexts");
    if (contexts == message.metadata.end() || !contexts->is_array()) return text;
    for (const auto& context : *contexts) {
        if (!context.is_object()) continue;
        const auto type = context.find("type");
        if (type == context.end() || !type->is_string() || *type != "page_annotations") continue;
        const auto pages = context.find("pages");
        if (pages == context.end() || !pages->is_array()) continue;
        for (const auto& page : *pages) {
            if (!page.is_object()) continue;
            const auto annotations = page.find("annotations");
            if (annotations == page.end() || !annotations->is_array()) continue;
            for (const auto& annotation : *annotations) {
                if (!annotation.is_object()) continue;
                const auto description = annotation.find("description");
                if (description == annotation.end() || !description->is_string()) continue;
                const auto& value = description->get_ref<const std::string&>();
                if (value.find_first_not_of(" \t\r\n") == std::string::npos) continue;
                if (!text.empty()) text += "\n";
                text += "Page annotation: " + value;
            }
        }
    }
    return text;
}

// 用户看到 / 输入的文字。content 里可能已经展开了 @文件 的内容或技能正文
// (那些是数据,不是用户的话),有 display_text 时以它为准。
std::string user_visible_text(const ChatMessage& message) {
    if (metadata_flag(message, "page_annotations_expanded")) return annotated_user_text(message);
    if (message.metadata.is_object()) {
        const auto it = message.metadata.find("display_text");
        if (it != message.metadata.end() && it->is_string() && !it->get_ref<const std::string&>().empty()) {
            return it->get<std::string>();
        }
    }
    return message.content;
}

bool is_inter_agent_task(const ChatMessage& message) {
    if (message.role != "user" || !message.metadata.is_object()) return false;
    const auto it = message.metadata.find("inter_agent");
    return it != message.metadata.end() && it->is_object() &&
           it->value("type", std::string{}) == "NEW_TASK";
}

// 提问工具的结果是不是用户本人答的:超时自动采纳、策略自动应答、插话、取消都不算。
bool is_user_answer(const ChatMessage& tool_message) {
    if (!tool_message.metadata.is_object()) return false;
    const auto& meta = tool_message.metadata;
    if (!meta.value("tool_success", false)) return false;
    if (meta.contains("ask_user_question_auto")) return false;
    const auto result = meta.find("ask_user_question_result");
    if (result == meta.end() || !result->is_object()) return false;
    if (result->value("interjected", false)) return false;
    const auto items = result->find("items");
    if (items == result->end() || !items->is_array() || items->empty()) return false;
    for (const auto& item : *items) {
        if (item.is_object() && item.value("auto_selected", false)) return false;
    }
    return true;
}

struct Entry {
    bool is_tool = false;
    std::string text;
    bool dropped = false;
};

} // namespace

bool is_user_authored_message(const ChatMessage& message) {
    if (!is_real_user_message(message)) return false;
    if (!message.subtype.empty()) return false;
    if (!message.metadata.is_object()) return true;
    // 真实用户消息上允许出现的布尔标记:回合中的插话、提问期间的插话、打断后
    // 重新提交、选区上下文已展开、网页批注已展开(这种消息只取用户写的部分,
    // 见 annotated_user_text)。其余为 true 的标记一律视为程序注入的记号。
    static constexpr const char* kBenignFlags[] = {
        "turn_steer", "question_interjection", "turn_interrupt", "selection_context_expanded",
        "page_annotations_expanded",
    };
    for (auto it = message.metadata.begin(); it != message.metadata.end(); ++it) {
        const std::string& key = it.key();
        // 其它 agent 派来的任务(NEW_TASK)对子 agent 而言等同请求,但不是用户的话;
        // source_tool 是工具执行后追加的提示。
        if (key == "inter_agent" || key == "source_tool") return false;
        if (!it->is_boolean() || !it->get<bool>()) continue;
        bool benign = false;
        for (const char* flag : kBenignFlags) {
            if (key == flag) { benign = true; break; }
        }
        if (!benign) return false;
    }
    return true;
}

std::string last_user_authored_id(const std::vector<ChatMessage>& history) {
    for (std::size_t i = history.size(); i > 0; --i) {
        const auto& message = history[i - 1];
        if (!is_user_authored_message(message)) continue;
        return message.uuid.empty() ? "#" + std::to_string(i - 1) : message.uuid;
    }
    return {};
}

std::string render_action(const ReviewAction& action, const TranscriptLimits& limits) {
    std::string out = entry("Tool " + action.tool_name + " ",
        project_arguments(action.arguments_json, limits.action_bytes * 2 / 3, limits.action_bytes));
    if (!action.execution.empty()) out += "Execution: " + action.execution + "\n";
    return out;
}

std::string render_transcript(const std::vector<ChatMessage>& history,
                              const ReviewAction& action,
                              const TranscriptOptions& options) {
    const TranscriptLimits& limits = options.limits;

    // 先收集每个调用的名字和结局,渲染时把结局接在调用后面。
    std::map<std::string, std::string> call_names;
    std::map<std::string, std::string> outcomes;
    for (const auto& message : history) {
        if (message.role == "assistant" && message.tool_calls.is_array()) {
            for (const auto& call : message.tool_calls) {
                if (!call.is_object() || !call.contains("function") || !call["function"].is_object()) continue;
                call_names[call.value("id", std::string{})] = call["function"].value("name", std::string{});
            }
        } else if (message.role == "tool" && !message.tool_call_id.empty()) {
            if (message.content.rfind(kUserDeniedPrefix, 0) == 0) {
                outcomes[message.tool_call_id] = "denied by the user";
            } else if (message.metadata.is_object()) {
                const auto review = message.metadata.find("auto_review");
                if (review != message.metadata.end() && review->is_object() &&
                    review->value("blocked", false)) {
                    outcomes[message.tool_call_id] = "blocked by auto mode";
                }
            }
        }
    }

    std::vector<Entry> entries;
    for (const auto& message : history) {
        if (message.role == "assistant") {
            if (!message.tool_calls.is_array()) continue;
            for (const auto& call : message.tool_calls) {
                if (!call.is_object() || !call.contains("function") || !call["function"].is_object()) continue;
                const std::string id = call.value("id", std::string{});
                // 待裁决的调用单独放进 <action>,这里不重复。
                if (!action.tool_call_id.empty() && id == action.tool_call_id) continue;
                const auto& function = call["function"];
                const std::string name = function.value("name", std::string{});
                if (name.empty()) continue;
                std::string arguments;
                if (function.contains("arguments")) {
                    arguments = function["arguments"].is_string()
                        ? function["arguments"].get<std::string>()
                        : function["arguments"].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                }
                Entry item;
                item.is_tool = true;
                item.text = entry("Tool " + name + " ",
                    project_arguments(arguments, limits.tool_arg_field_bytes, limits.tool_call_bytes));
                const auto outcome = outcomes.find(id);
                if (outcome != outcomes.end()) item.text += "  -> " + outcome->second + "\n";
                entries.push_back(std::move(item));
            }
            continue;
        }
        if (message.role == "tool") {
            const auto name = call_names.find(message.tool_call_id);
            if (name != call_names.end() && name->second == kAskToolName && is_user_answer(message)) {
                entries.push_back({false, entry("User answered a question: ",
                    clip_middle(message.content, limits.user_message_bytes)), false});
            }
            continue;   // 其它工具输出一律不进记录。
        }
        if (message.role != "user") continue;
        if (metadata_flag(message, "turn_interrupt_marker")) {
            entries.push_back({false, "Note: the user interrupted the agent here.\n", false});
            continue;
        }
        if (is_compact_summary_message(message)) {
            entries.push_back({false, entry(
                "Summary of earlier conversation (written by a model, not the user's words): ",
                clip_middle(strip_summary_prefix(message.content), limits.summary_bytes)), false});
            continue;
        }
        if (is_inter_agent_task(message)) {
            entries.push_back({false, entry("Task from another agent (not the user): ",
                clip_middle(message.content, limits.summary_bytes)), false});
            continue;
        }
        if (!is_user_authored_message(message)) continue;
        const std::string text = clip_middle(user_visible_text(message), limits.user_message_bytes);
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        entries.push_back({false, entry(
            options.group_delegate ? "Group member (not the owner): " : "User: ", text), false});
    }

    std::size_t total = 0;
    for (const auto& item : entries) total += item.text.size();

    // 超预算:先丢最早的工具调用,再丢最早的其它条目;最后一条始终保留。
    std::size_t dropped_tools = 0;
    std::size_t dropped_others = 0;
    for (int pass = 0; pass < 2 && total > limits.transcript_bytes; ++pass) {
        for (std::size_t i = 0; i + 1 < entries.size() && total > limits.transcript_bytes; ++i) {
            auto& item = entries[i];
            if (item.dropped || item.is_tool != (pass == 0)) continue;
            item.dropped = true;
            total -= item.text.size();
            ++(pass == 0 ? dropped_tools : dropped_others);
        }
    }

    std::string out;
    bool note_written = false;
    for (const auto& item : entries) {
        if (item.dropped) {
            if (note_written) continue;
            note_written = true;
            out += "[...";
            if (dropped_tools > 0) out += " " + std::to_string(dropped_tools) + " earlier tool calls";
            if (dropped_tools > 0 && dropped_others > 0) out += " and";
            if (dropped_others > 0) out += " " + std::to_string(dropped_others) + " earlier messages";
            out += " omitted ...]\n";
            continue;
        }
        out += item.text;
    }
    if (out.empty()) out = "(no earlier activity in this session)\n";
    return out;
}

} // namespace acecode::auto_review
