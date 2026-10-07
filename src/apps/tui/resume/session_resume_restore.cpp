#include "session_resume_restore.hpp"

#include "session/compact_checkpoint.hpp"
#include "session/request_context_record.hpp"
#include "session_replay.hpp"
#include "tool/file_state_restore.hpp"
#include "session/session_rewind.hpp"
#include "session/tool_result_storage.hpp"
#include "session/turn_timing.hpp"
#include "agent/agent_loop.hpp"
#include "tool/tool_executor.hpp"
#include "tui/tui_state.hpp"

#include <map>
#include <optional>

namespace acecode {

namespace {

bool is_llm_role(const std::string& role) {
    return role == "user" || role == "assistant" ||
           role == "system" || role == "tool";
}

bool is_transcript_only_message(const ChatMessage& msg) {
    return msg.metadata.is_object() &&
           msg.metadata.value("transcript_only", false);
}

std::optional<std::pair<std::size_t, CompactCheckpoint>>
latest_valid_compact_checkpoint(const std::vector<ChatMessage>& messages) {
    for (std::size_t i = messages.size(); i > 0; --i) {
        auto checkpoint = decode_compact_checkpoint(messages[i - 1]);
        if (checkpoint.has_value()) {
            return std::make_pair(i - 1, std::move(*checkpoint));
        }
    }
    return std::nullopt;
}

void append_agent_model_messages(const std::vector<ChatMessage>& messages,
                                 AgentLoop& agent_loop) {
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (is_request_context_metadata(msg) && !is_request_context_record(msg)) continue;
        if (is_file_checkpoint_message(msg)) continue;
        if (is_content_replacement_message(msg)) continue;
        if (is_turn_timing_message(msg)) continue;
        if (is_compact_checkpoint_message(msg)) continue;

        const bool is_shell_user =
            (msg.role == "user" && !msg.content.empty() && msg.content[0] == '!');
        const bool next_is_result =
            (i + 1 < messages.size() && messages[i + 1].role == "tool_result");
        if (is_shell_user && next_is_result) {
            agent_loop.inject_shell_turn(msg.content.substr(1),
                                         messages[i + 1].content,
                                         "",
                                         0);
            ++i;
            continue;
        }

        if (is_llm_role(msg.role) && !is_transcript_only_message(msg)) {
            agent_loop.push_message(msg);
        }
    }
}

} // namespace

void append_resumed_session_messages(const std::vector<ChatMessage>& messages,
                                     TuiState& state,
                                     AgentLoop& agent_loop,
                                     const ToolExecutor& tools) {
    restore_file_tool_state_from_messages(messages, agent_loop.cwd());

    agent_loop.clear_messages();
    if (auto checkpoint = latest_valid_compact_checkpoint(messages)) {
        append_agent_model_messages(checkpoint->second.replacement_history, agent_loop);
        if (checkpoint->first + 1 < messages.size()) {
            std::vector<ChatMessage> suffix(
                messages.begin() + static_cast<std::ptrdiff_t>(checkpoint->first + 1),
                messages.end());
            append_agent_model_messages(suffix, agent_loop);
        }
    } else {
        append_agent_model_messages(messages, agent_loop);
    }

    std::vector<ChatMessage> replay_buffer;
    auto flush_replay = [&]() {
        if (replay_buffer.empty()) return;
        auto rows = replay_session_messages(replay_buffer, tools);
        for (auto& row : rows) {
            state.conversation.push_back(std::move(row));
        }
        replay_buffer.clear();
    };

    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (is_file_checkpoint_message(msg)) {
            continue;
        }
        if (is_content_replacement_message(msg)) {
            continue;
        }
        if (is_turn_timing_message(msg)) {
            continue;
        }
        if (is_compact_checkpoint_message(msg)) {
            continue;
        }

        // Shell mode persists a UI-only pair: user content starts with '!'
        // followed by a `tool_result` pseudo-role. The TUI should show the pair
        // as-is, while the LLM context gets the XML-tagged shell turn via
        // AgentLoop::inject_shell_turn.
        bool is_shell_user =
            (msg.role == "user" && !msg.content.empty() && msg.content[0] == '!');
        bool next_is_result =
            (i + 1 < messages.size() && messages[i + 1].role == "tool_result");
        if (is_shell_user && next_is_result) {
            flush_replay();
            state.conversation.push_back({msg.role, msg.content, false});
            // 把落盘的 "tool_result"(伪角色)翻译为运行时使用的
            // "user_shell_output",让 resume 后的 chat 视图与实时 `!cmd` 行为
            // 一致——全量显示用户主动跑的命令输出,不走 fold/summary 路径。
            TuiState::Message shell_row;
            shell_row.role = "user_shell_output";
            shell_row.content = messages[i + 1].content;
            shell_row.is_tool = true;
            state.conversation.push_back(std::move(shell_row));
            ++i;
            continue;
        }

        replay_buffer.push_back(msg);
    }

    flush_replay();
}

} // namespace acecode
