#include "tui/app/tui_agent_bridge.hpp"
#include "session/compact_notice.hpp"
#include "session/inter_agent_message.hpp"
#include "tui/app/tui_screen_host.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"
#include "tui/model/user_turn_state.hpp"
#include "tui/model/thinking_phrases.hpp"
#include "tui/model_retry_status.hpp"
#include "tui/compact_notice_row.hpp"
#include "tui/tool_row_format.hpp"
#include "tui/thinking_animation.hpp"
#include "agent/compaction/compact.hpp"
#include "session/token_tracker.hpp"
#include "tool/tool_executor.hpp"
#include "llm/text_preamble_tags.hpp"
#include "platform/power_inhibitor.hpp"
#include "config/config.hpp"
using ftxui::Event;
namespace acecode::tui {
TuiAgentBridge::TuiAgentBridge(TuiState& s, TuiScreenHost& host, ChatViewport& v,
    TokenTracker& tracker, AppConfig& cfg, TurnObservation& obs)
    : state(s), screen_host(host), screen(host), viewport(v), token_tracker(tracker),
      config(cfg), observation(obs) {}
AgentCallbacks TuiAgentBridge::initial_callbacks() {
    AgentCallbacks callbacks;
    callbacks.on_message = bind(&TuiAgentBridge::on_message);
    callbacks.on_transcript_message = bind(&TuiAgentBridge::on_transcript_message);
    callbacks.on_busy_changed = bind(&TuiAgentBridge::on_busy_changed);
    callbacks.on_delta = bind(&TuiAgentBridge::on_delta);
    callbacks.on_tool_result = bind(&TuiAgentBridge::on_tool_result);
    callbacks.on_usage = bind(&TuiAgentBridge::on_usage);
    callbacks.on_goal_status = bind(&TuiAgentBridge::on_goal_status);
    callbacks.on_todo_updated = bind(&TuiAgentBridge::on_todo_updated);
    callbacks.on_thinking_title = bind(&TuiAgentBridge::on_thinking_title);
    callbacks.on_transcript_replace = bind(&TuiAgentBridge::on_transcript_replace);
    callbacks.on_stream_retry_reset = bind(&TuiAgentBridge::on_stream_retry_reset);
    callbacks.on_model_retry = bind(&TuiAgentBridge::on_model_retry);
    callbacks.on_model_retry_resume = bind(&TuiAgentBridge::on_model_retry_resume);
    callbacks.on_turn_finished = bind(&TuiAgentBridge::on_turn_finished);
    return callbacks;
}
void TuiAgentBridge::install_progress_callbacks(AgentCallbacks& callbacks) {
    callbacks.on_tool_progress_start = bind(&TuiAgentBridge::on_tool_progress_start);
    callbacks.on_tool_progress_update = bind(&TuiAgentBridge::on_tool_progress_update);
    callbacks.on_tool_progress_end = bind(&TuiAgentBridge::on_tool_progress_end);
}
void TuiAgentBridge::on_message(const std::string& role, const std::string& raw_content, bool is_tool) {
    // 工具前言(add-tool-preamble):assistant 正文里的 <text_preamble> 标签
    // 只在实时期间进 loading,不进 transcript;整段都是标签时不建空行。
    const bool assistant_text = !is_tool && role == "assistant";
    const std::string content = assistant_text
        ? acecode::llm::strip_text_preamble_tags(raw_content)
        : raw_content;
    if (assistant_text && content.empty()) return;
    std::lock_guard<std::mutex> lk(state.mu);
    if (!is_tool && role == "assistant") {
        observation.assistant_text = content;
    }
    if (!is_tool && role == "assistant" &&
        !state.conversation.empty() &&
        state.conversation.back().role == "assistant" &&
        !state.conversation.back().is_tool) {
        state.conversation.back().content = content;
    } else {
        TuiState::Message m{role, content, is_tool};
        if (role == "tool_call") {
            // push 时就算好紧凑预览:工具执行期间行内即显示
            // `● Bash(npm install)` 而非原始 JSON;完成后 on_tool_result
            // 的补挂对已有值是 no-op。
            const auto parts =
                acecode::tui::parse_tool_row(content, std::string());
            if (!parts.name.empty()) {
                m.display_override = ToolExecutor::build_tool_call_preview(
                    parts.name, parts.args);
            }
        }
        state.conversation.push_back(std::move(m));
    }
    viewport.clamp_focus(state);
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_transcript_message(const ChatMessage& message) {
    std::lock_guard<std::mutex> lk(state.mu);
    const auto notice = decode_compact_notice(message);
    if (notice.has_value() && notice->stage == "progress") {
        state.is_compacting = true;
        state.compact_animation_start_time =
            std::chrono::steady_clock::now();
    }

    // 蜂群模式（网状）的 agent 间消息落盘为 user 信封,界面显示为系统行。
    const std::string inter_agent = acecode::mesh::inter_agent_display_text(message);
    if (!inter_agent.empty()) {
        state.conversation.push_back({"system", inter_agent, false});
    } else if (!acecode::tui::append_compact_notice_row(
            state.conversation, message)) {
        state.conversation.push_back(
            {message.role, message.content, false});
    }
    if (notice.has_value() && notice->complete) {
        state.is_compacting = false;
    }

    viewport.reset(state);
    state.chat_follow_tail = true;
    viewport.clamp_focus(state);
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_busy_changed(bool busy) {
    acecode::note_process_session_busy(kTuiMainPowerSessionId, busy);
    std::lock_guard<std::mutex> lk(state.mu);
    if (busy && !state.is_waiting) {
        tui::begin_user_turn_locked(state, tui::UserTurnPhrase::Random, tui::WaitingUpdate::Preserve);
    }
    state.is_waiting = busy;
    if (!busy) state.is_compacting = false;
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_delta(const std::string& token) {
    {
        std::lock_guard<std::mutex> lk(state.mu);
        // Find or create the streaming assistant message
        if (state.conversation.empty() ||
            state.conversation.back().role != "assistant" ||
            state.conversation.back().is_tool) {
            state.conversation.push_back({"assistant", "", false});
        }
        state.conversation.back().content += token;
        state.streaming_output_chars += token.size();
        viewport.clamp_focus(state);
    }
    const std::int64_t now_ms = tui::monotonic_milliseconds();
    const bool keyboard_input_recent =
        acecode::tui::is_keyboard_input_recent(
            now_ms,
            screen_host.last_keyboard_input_at_ms().load(std::memory_order_acquire));
    screen_host.request_scheduled_redraw(
        acecode::tui::select_streaming_redraw_interval_ms(
            keyboard_input_recent,
            screen_host.redraw_pacer()->last_frame_latency_ms()));
}
void TuiAgentBridge::on_tool_result(const ChatMessage& call_msg, const std::string& tool_name, const ToolResult& result) {
    std::lock_guard<std::mutex> lk(state.mu);
    // Walk the tail backwards: most recent tool_result gets `summary` +
    // `hunks`, the nearest preceding tool_call gets `display_override`.
    // Both were just pushed by `on_message` on the agent worker thread.
    for (auto it = state.conversation.rbegin(); it != state.conversation.rend(); ++it) {
        if (it->role == "tool_result" && !it->summary.has_value() &&
            !it->ask_result) {
            it->summary = result.summary;
            it->hunks = result.hunks;
            it->ask_result = tool_name == "AskUserQuestion";
            const int index = static_cast<int>(
                state.conversation.size() - 1 -
                static_cast<std::size_t>(
                    it - state.conversation.rbegin()));
            viewport.invalidate(index);
            break;
        }
    }
    if (!call_msg.display_override.empty()) {
        for (auto it = state.conversation.rbegin(); it != state.conversation.rend(); ++it) {
            if (it->role == "tool_call" && it->display_override.empty()) {
                it->display_override = call_msg.display_override;
                break;
            }
        }
    }
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_usage(const TokenUsage& usage) {
    token_tracker.record(usage);
    std::lock_guard<std::mutex> lk(state.mu);
    state.token_status = token_tracker.format_status(config.context_window);
    state.token_percent = token_tracker.context_percent(config.context_window);
    state.cache_hit_percent = token_tracker.cache_hit_percent();
    // 心跳读数走回合累计:本请求的 completion_tokens 入账,同时清零流式
    // 估算基数(该请求的 delta 已计入确认值,不清会双重计数)。
    state.turn_completion_tokens_confirmed += usage.completion_tokens;
    state.streaming_output_chars = 0;
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_goal_status(const std::string& status) {
    std::lock_guard<std::mutex> lk(state.mu);
    state.goal_status = status;
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_todo_updated(const nlohmann::json& payload) {
    std::lock_guard<std::mutex> lk(state.mu);
    if (payload.is_object() && payload.contains("todos")) {
        state.todos = todo_items_from_json(payload["todos"]);
    } else {
        state.todos.clear();
    }
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_thinking_title(const std::string& title) {
    if (title.empty()) return;
    std::lock_guard<std::mutex> lk(state.mu);
    state.current_thinking_phrase = title;
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_transcript_replace(const std::vector<ChatMessage>& /*messages*/, const CompactResult& result) {
    if (!result.performed || result.summary_text.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lk(state.mu);
    state.conversation.push_back({"system", "--- [Compact Checkpoint] ---", false});
    state.conversation.push_back({"system", "[Conversation summary]\n" + result.summary_text, false});
    viewport.reset(state);
    state.chat_follow_tail = true;
    viewport.clamp_focus(state);
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_stream_retry_reset() {
    std::lock_guard<std::mutex> lk(state.mu);
    if (!state.conversation.empty() &&
        state.conversation.back().role == "assistant" &&
        !state.conversation.back().is_tool) {
        state.conversation.pop_back();
    }
    observation.assistant_text.clear();
    state.streaming_output_chars = 0;
    viewport.clamp_focus(state);
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_model_retry(const ProviderErrorInfo& info) {
    std::lock_guard<std::mutex> lk(state.mu);
    state.current_thinking_phrase =
        acecode::tui::model_retry_wait_phrase(
            tui::is_user_chinese(state), info.retry_delay_ms);
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_model_retry_resume() {
    std::lock_guard<std::mutex> lk(state.mu);
    state.current_thinking_phrase =
        acecode::tui::model_retry_resume_phrase(tui::is_user_chinese(state));
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_turn_finished(const std::string& status) {
    std::lock_guard<std::mutex> lk(state.mu);
    observation.outcome = status;
}
void TuiAgentBridge::on_tool_progress_start(const std::string& tool_name, const std::string& cmd_preview, const std::string& preamble) {
    {
        std::lock_guard<std::mutex> lk(state.mu);
        state.tool_running = true;
        state.tool_progress = {};
        state.tool_progress.tool_name = tool_name;
        state.tool_progress.command_preview = cmd_preview;
        state.tool_progress.preamble = preamble;
        state.tool_progress.start_time = std::chrono::steady_clock::now();
        state.last_tool_post_event_time = std::chrono::steady_clock::now();
    }
    screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_tool_progress_update(const std::vector<std::string>& tail_snapshot, const std::string& current_partial, size_t total_bytes, int total_lines) {
    bool should_post = false;
    {
        std::lock_guard<std::mutex> lk(state.mu);
        state.tool_progress.tail_lines = tail_snapshot;
        state.tool_progress.current_partial = current_partial;
        state.tool_progress.total_bytes = total_bytes;
        state.tool_progress.total_lines = total_lines;
        auto now = std::chrono::steady_clock::now();
        if (now - state.last_tool_post_event_time > std::chrono::milliseconds(150)) {
            state.last_tool_post_event_time = now;
            should_post = true;
        }
    }
    if (should_post) screen.post_event(Event::Custom);
}
void TuiAgentBridge::on_tool_progress_end() {
    {
        std::lock_guard<std::mutex> lk(state.mu);
        state.tool_running = false;
        state.tool_progress = {};
    }
    // Unconditional PostEvent so the live element disappears immediately.
    screen.post_event(Event::Custom);
}
}
