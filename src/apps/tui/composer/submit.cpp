#include "tui/model/user_turn_state.hpp"
#include "tui/composer/submit.hpp"
#include "tui/composer/suggestions.hpp"
#include "tui/overlays/list_picker_input.hpp"
#include "tui/paste_handler.hpp"
#include "tui/pending_attachment_selection.hpp"
#include "tui/model/status_line.hpp"
#include "tui/model/thinking_phrases.hpp"
#include "tui/commands/command_registry.hpp"
#include "session/composer_attachments.hpp"
#include "session/session_storage.hpp"
#include "history/input_history_recorder.hpp"
#include "config/config.hpp"
using ftxui::Event;

namespace acecode::tui {
InputDisposition handle_composer_submit(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    auto& screen = ctx.screen;
    auto& viewport = ctx.viewport;
    auto& cmd_registry = ctx.commands;
    auto& config = ctx.config;
    auto& auth_done = ctx.auth_done;
    auto& working_dir = ctx.working_dir;
    if (event == Event::Return) {
        std::unique_lock<std::mutex> lk(state.mu);

        if (auto result = tui::input_result(tui::list_picker_enter_locked(state, screen, viewport))) return input_stopped(*result);

        // confirm_pending 现在由上面的 confirm overlay handler 单独拦截
        // (Enter 直接走那条路径),这里不会再被 confirm 触发。

        if (state.input_text.empty() && state.pending_attachments.empty()) return InputDisposition::Consumed;
        if (!auth_done) return InputDisposition::Consumed;

        // Block message submission during compaction
        if (state.is_compacting) return InputDisposition::Consumed;

        // 多行粘贴折叠（fix-multiline-paste-input change）：把已知 [Pasted text #N]
        // 占位符替换回 pasted_texts 中存的全文，得到 expanded_prompt 喂给 agent_loop
        // / 斜杠命令 / pending 队列 / input_history / 对话气泡。提交后清空 input_text
        // 与 pasted_texts；next_paste_id 不复位（display 用计数，跨提交单调递增
        // 即可，避免极端情况下与 input_history 中残留占位符同号撞 ID）。
        //
        // 用户反馈：提交后对话气泡也显示展开版原文，而非紧凑占位符——所以这里
        // 没有保留单独的 `visible_prompt`，提交即一并展开上屏。
        const std::string expanded_prompt = acecode::tui::expand_placeholders(
            state.input_text, state.pasted_texts);
        const std::vector<nlohmann::json> attachments = state.pending_attachments;
        const std::string display_prompt =
            display_prompt_with_attachments(expanded_prompt, attachments);
        state.input_text.clear(); state.pasted_texts.clear();
        state.pending_attachments.clear();
        state.pending_attachment_focus =
            acecode::tui::kNoPendingAttachmentFocus;
        state.input_cursor = 0;
        state.clear_input_selection();
        tui::refresh_input_suggestions(state, cmd_registry, ctx.turn.cwd());

        // 统一入口：内存 push + 磁盘 append。空白 / 相邻重复被抑制，保持磁盘与内存
        // 行为一致；磁盘持久化受 config.input_history.enabled 控制。

        // Shell input mode: dispatch directly to BashTool via agent worker.
        // Skips slash-command parsing and LLM round-trip.
        if (state.input_mode == InputMode::Shell) {
            if (!attachments.empty()) {
                tui::set_transient_status_line_locked(
                    state,
                    "Image attachments are only supported in normal prompt mode");
                state.input_text = expanded_prompt;
                state.input_cursor = state.input_text.size();
                state.clear_input_selection();
                state.pending_attachments = attachments;
                acecode::tui::clamp_pending_attachment_focus(
                    state.pending_attachment_focus,
                    state.pending_attachments.size());
                screen.post_event(Event::Custom);
                return InputDisposition::Consumed;
            }
            const std::string shell_cmd = expanded_prompt;
            record_input_history(state.input_history, config.input_history, SessionStorage::get_project_dir(working_dir), prepend_mode_prefix(shell_cmd, InputMode::Shell));
            state.history_index = -1;
            state.input_mode = InputMode::Normal;

            // 提交后对话气泡显示展开后的原文（用户反馈：上屏不要看到 [] 占位符）。
            state.conversation.push_back({"user", "!" + shell_cmd, false});
            state.chat_follow_tail = true;
            viewport.clamp_focus(state);
            tui::begin_user_turn_locked(state, tui::UserTurnPhrase::Shell, tui::WaitingUpdate::SetTrue);
            ctx.turn.submit_shell(shell_cmd);
            return InputDisposition::Consumed;
        }

        // Record history（用 expanded_prompt：上箭头取回原文，再次提交不会发出
        // 字面 [Pasted text #N] —— 因为本会话提交后 store 已经清空，未展开的
        // 字面占位符在下次 submit 时也会按 unknown id 保留，丢失原文。）
        record_input_history(state.input_history, config.input_history, SessionStorage::get_project_dir(working_dir), expanded_prompt);
        state.history_index = -1;

        // Slash command interception（用 expanded_prompt 派发：spec 4.3）。
        if (attachments.empty() && !expanded_prompt.empty() && expanded_prompt[0] == '/') {
            CommandContext cmd_ctx = ctx.command_contexts.make(true);
            const size_t before_command_messages =
                state.conversation.size();
            lk.unlock();
            bool handled = cmd_registry.dispatch(expanded_prompt, cmd_ctx);
            if (handled) {
                lk.lock();
                if (state.conversation.size() != before_command_messages) {
                    viewport.reset(state);
                }
                viewport.clamp_focus(state);
                screen.post_event(Event::Custom);
                return InputDisposition::Consumed;
            }
            lk.lock();
            // If not a known command, fall through to send as normal prompt
        }

        if (state.is_waiting) {
            // 队列保存展开版（spec 4.5）；提交后气泡显示展开版（用户反馈：
            // 上屏不要看到 [] 占位符）。
            state.pending_queue.push_back(display_prompt);
            if (!attachments.empty()) {
                state.pending_structured_queue.push_back(
                    build_user_input_with_attachments(
                        expanded_prompt, display_prompt, attachments));
            }
        } else {
            lk.unlock();
            ctx.turn.before_first_turn();
            lk.lock();
            state.conversation.push_back({"user", display_prompt, false});
            state.chat_follow_tail = true;
            viewport.clamp_focus(state);
            tui::begin_user_turn_locked(state, tui::UserTurnPhrase::Random, tui::WaitingUpdate::SetTrue);
            if (attachments.empty()) {
                ctx.turn.submit_text(expanded_prompt);
            } else {
                ctx.turn.submit_input(build_user_input_with_attachments(
                    expanded_prompt, display_prompt, attachments));
            }
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

}
