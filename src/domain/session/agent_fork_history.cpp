#include "agent_fork_history.hpp"

#include "inter_agent_message.hpp"
#include "llm/message_predicates.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::mesh {

namespace {

constexpr const char* kForkTurnsError =
    "fork_turns must be `none`, `all`, or a positive integer string";

std::string trim_lower(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    std::string out = text.substr(begin, end - begin);
    for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

bool keep_for_fork(const ChatMessage& message) {
    if (message.role == "user") {
        if (is_inter_agent_message(message)) return false;
        return is_real_user_message(message) || is_compact_summary_message(message);
    }
    if (message.role == "assistant") {
        const bool has_tool_calls = !message.tool_calls.is_null() && !message.tool_calls.empty();
        return !has_tool_calls && !message.content.empty();
    }
    return false;
}

// Codex fork-turn boundaries: real user messages and NEW_TASK (trigger) mail.
bool starts_fork_turn(const ChatMessage& message) {
    if (message.role != "user") return false;
    if (const auto envelope = inter_agent_envelope_from_metadata(message.metadata)) {
        return envelope->type == InterAgentMessageType::NewTask;
    }
    return is_real_user_message(message);
}

} // namespace

std::optional<ForkTurns> parse_fork_turns(const std::string& text, std::string* error) {
    const std::string value = trim_lower(text);
    ForkTurns fork;
    if (value.empty() || value == "all") return fork;
    if (value == "none") {
        fork.kind = ForkTurns::Kind::None;
        return fork;
    }
    const bool digits = std::all_of(value.begin(), value.end(),
                                    [](char ch) { return ch >= '0' && ch <= '9'; });
    std::size_t parsed = 0;
    if (digits && value.size() <= 9) parsed = static_cast<std::size_t>(std::stoul(value));
    if (!digits || parsed == 0) {
        if (error) *error = kForkTurnsError;
        return std::nullopt;
    }
    fork.kind = ForkTurns::Kind::LastN;
    fork.last_n = parsed;
    return fork;
}

std::vector<ChatMessage> build_fork_history(const std::vector<ChatMessage>& effective_history,
                                            const ForkTurns& fork) {
    std::vector<ChatMessage> kept;
    if (fork.kind == ForkTurns::Kind::None) return kept;
    std::size_t begin = 0;
    if (fork.kind == ForkTurns::Kind::LastN) {
        // Truncate first, then filter: with fewer than N boundaries keep from
        // the first one; with none the fork is empty.
        std::vector<std::size_t> starts;
        for (std::size_t i = 0; i < effective_history.size(); ++i) {
            if (starts_fork_turn(effective_history[i])) starts.push_back(i);
        }
        if (starts.empty()) return kept;
        begin = starts.size() > fork.last_n ? starts[starts.size() - fork.last_n] : starts.front();
    }
    for (std::size_t i = begin; i < effective_history.size(); ++i) {
        const auto& message = effective_history[i];
        if (!keep_for_fork(message)) continue;
        ChatMessage copy = message;
        copy.reasoning_content.clear();
        copy.display_override.clear();
        if (!copy.metadata.is_object()) copy.metadata = nlohmann::json::object();
        copy.metadata["mesh_inherited"] = true;
        kept.push_back(std::move(copy));
    }
    return kept;
}

} // namespace acecode::mesh
