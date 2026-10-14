#include "session_history_recovery.hpp"

#include "llm/tool_call_corruption.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace acecode {

namespace {

constexpr const char* kInterruptedToolResult =
    "[Interrupted] ACECode recovered this session after the tool call ended "
    "without a persisted result. The execution outcome is unknown. Inspect "
    "the current state before deciding whether to retry; do not assume success "
    "or failure.";

bool has_assistant_content(const ChatMessage& msg) {
    return !msg.content.empty() ||
           !msg.reasoning_content.empty() ||
           (msg.content_parts.is_array() && !msg.content_parts.empty());
}

std::optional<std::string> tool_call_id(const nlohmann::json& call) {
    if (!call.is_object() ||
        !call.contains("id") ||
        !call["id"].is_string()) {
        return std::nullopt;
    }
    std::string id = call["id"].get<std::string>();
    if (id.empty()) return std::nullopt;
    return id;
}

std::optional<std::string> valid_tool_call_id(const nlohmann::json& call) {
    auto id = tool_call_id(call);
    if (!id ||
        !call.contains("function") ||
        !call["function"].is_object()) {
        return std::nullopt;
    }
    const auto& function = call["function"];
    // 空名字与损坏的名字(调用表达式、夹着模板标记)都不发给模型:原样排回
    // 提示词等于给模型看错误示范。
    if (!function.contains("name") ||
        !function["name"].is_string() ||
        diagnose_corrupted_tool_name(
            function["name"].get_ref<const std::string&>())) {
        return std::nullopt;
    }
    return id;
}

ChatMessage malformed_tool_call_note() {
    ChatMessage note;
    note.role = "user";
    note.content = kMalformedToolCallNote;
    // hidden_goal_context:内部上下文,不算真实用户回合的起点,界面也不显示。
    note.metadata = nlohmann::json{
        {"hidden_goal_context", true},
        {"session_recovery", {
            {"version", 1},
            {"kind", "malformed_tool_call"},
            {"outcome", "not_executed"},
        }},
    };
    return note;
}

bool is_malformed_tool_call_note(const ChatMessage& msg) {
    return msg.role == "user" && msg.content == kMalformedToolCallNote;
}

ChatMessage interrupted_tool_result(const std::string& tool_call_id) {
    ChatMessage result;
    result.role = "tool";
    result.tool_call_id = tool_call_id;
    result.content = kInterruptedToolResult;
    result.metadata = nlohmann::json{
        {"session_recovery", {
            {"version", 1},
            {"kind", "missing_tool_result"},
            {"outcome", "unknown"},
        }},
    };
    return result;
}

} // namespace

ProviderHistoryRecoveryResult recover_provider_history(
    const std::vector<ChatMessage>& messages) {
    ProviderHistoryRecoveryResult result;
    result.messages.reserve(messages.size());

    // 预留整个输入的 ID，避免改名占用后续调用或误匹配无关联结果。
    std::unordered_set<std::string> reserved_ids;
    const auto reserve_call_id = [&](const nlohmann::json& call) {
        const auto id = valid_tool_call_id(call);
        if (id) reserved_ids.insert(*id);
    };
    for (const auto& msg : messages) {
        if (msg.role == "assistant") {
            if (msg.tool_calls.is_array()) {
                for (const auto& call : msg.tool_calls) reserve_call_id(call);
            } else if (msg.tool_calls.is_object()) {
                reserve_call_id(msg.tool_calls);
            }
        } else if (msg.role == "tool" && !msg.tool_call_id.empty()) {
            reserved_ids.insert(msg.tool_call_id);
        }
    }
    std::size_t next_recovered_id = 0;

    std::vector<std::string> pending_order;
    // 当前 assistant 的原始 ID -> 提供者投影 ID；结果只在本组内配对。
    std::unordered_map<std::string, std::string> pending_ids;
    std::unordered_set<std::string> seen_call_ids;
    std::unordered_set<std::string> seen_result_ids;
    // 本组里被丢掉的坏调用(空名字 / 损坏的名字)的 ID。它们的报错结果不能作为
    // tool 消息发出(没有对应的调用),也不能直接丢:内网 GLM 会话里空名调用连同
    // 报错一起被删,请求以孤立的 assistant 消息结尾,模型看不到失败,原样重复。
    std::unordered_set<std::string> malformed_ids;
    bool malformed_note_pending = false;

    const auto flush_missing_results = [&]() {
        for (const auto& id : pending_order) {
            const auto pending = pending_ids.find(id);
            if (pending == pending_ids.end()) continue;
            result.messages.push_back(interrupted_tool_result(pending->second));
            seen_result_ids.insert(id);
            ++result.stats.synthesized_tool_results;
        }
        pending_order.clear();
        pending_ids.clear();
        // 说明放在本组全部工具结果之后,不拆开 assistant(tool_calls) 与结果的配对。
        // 相邻的同一说明只留一条:旧会话里连续十几轮坏调用不必重复十几遍。
        if (malformed_note_pending &&
            (result.messages.empty() ||
             !is_malformed_tool_call_note(result.messages.back()))) {
            result.messages.push_back(malformed_tool_call_note());
        }
        malformed_note_pending = false;
        malformed_ids.clear();
    };

    for (const auto& original : messages) {
        // System rows are movable provider context, not a turn boundary.
        // Keeping the pending set open lets a real tool result that follows an
        // injected system row win over a synthetic interruption placeholder.
        if (original.role != "tool" && original.role != "system") {
            flush_missing_results();
        }

        if (original.role == "assistant") {
            ChatMessage assistant = original;
            nlohmann::json calls = nlohmann::json::array();
            if (original.tool_calls.is_array()) {
                calls = original.tool_calls;
            } else if (original.tool_calls.is_object()) {
                calls.push_back(original.tool_calls);
            }

            nlohmann::json retained = nlohmann::json::array();
            std::unordered_set<std::string> batch_ids;
            for (const auto& call : calls) {
                const auto id = valid_tool_call_id(call);
                if (!id.has_value()) {
                    ++result.stats.malformed_tool_calls;
                    if (auto bad_id = tool_call_id(call)) {
                        malformed_ids.insert(std::move(*bad_id));
                    }
                    continue;
                }
                if (!batch_ids.insert(*id).second) {
                    ++result.stats.duplicate_tool_calls;
                    continue;
                }
                auto projected_call = call;
                std::string projected_id = *id;
                if (!seen_call_ids.insert(*id).second) {
                    // 兼容服务可能每次响应都返回 call_0；跨消息复用不是重复执行。
                    ++result.stats.duplicate_tool_calls;
                    do {
                        projected_id = "call_ace_recovered_" +
                                       std::to_string(++next_recovered_id);
                    } while (!reserved_ids.insert(projected_id).second);
                    projected_call["id"] = projected_id;
                }
                retained.push_back(std::move(projected_call));
                pending_order.push_back(*id);
                pending_ids.emplace(*id, std::move(projected_id));
            }

            if (retained.empty()) {
                assistant.tool_calls = nlohmann::json();
                if (!has_assistant_content(assistant)) {
                    ++result.stats.empty_assistant_messages;
                    continue;
                }
            } else {
                assistant.tool_calls = std::move(retained);
            }
            result.messages.push_back(std::move(assistant));
            continue;
        }

        if (original.role == "tool") {
            const std::string& id = original.tool_call_id;
            const auto pending = pending_ids.find(id);
            if (pending != pending_ids.end()) {
                ChatMessage tool = original;
                tool.tool_call_id = pending->second;
                result.messages.push_back(std::move(tool));
                pending_ids.erase(pending);
                seen_result_ids.insert(id);
                continue;
            }
            if (malformed_ids.erase(id) > 0) {
                ++result.stats.malformed_call_results;
                malformed_note_pending = true;
                continue;
            }
            if (!id.empty() && seen_result_ids.count(id)) {
                ++result.stats.duplicate_tool_results;
            } else if (pending_ids.empty()) {
                ++result.stats.standalone_tool_results;
            } else {
                ++result.stats.unexpected_tool_results;
            }
            continue;
        }

        result.messages.push_back(original);
    }

    flush_missing_results();
    return result;
}

} // namespace acecode
