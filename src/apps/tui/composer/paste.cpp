#include "tui/composer/paste.hpp"
#include "tui/composer/suggestions.hpp"
#include "tui/model/input_state.hpp"
#include "tui/model/status_line.hpp"
#include "tui/overlays/ask_session_projection.hpp"
#include "tui/text_input_ops.hpp"
#include "tui/paste_handler.hpp"
#include "tui/pending_attachment_selection.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/attachment_store.hpp"
#include "tui/commands/command_registry.hpp"
using ftxui::Event;

namespace acecode::tui {
void insert_pasted_text_at_cursor_locked(TuiState& state,
                                                const std::string& normalized) {
    cancel_ctrl_c_exit_locked(state);
    acecode::erase_text_selection(
        state.input_text,
        state.input_cursor,
        state.input_selection_anchor);
    acecode::tui::prune_unreferenced(
        state.pasted_texts, state.input_text);
    state.input_cursor = acecode::clamp_utf8_boundary(
        state.input_text, state.input_cursor);
    state.input_vertical_goal_column.reset();
    state.pending_attachment_focus =
        acecode::tui::kNoPendingAttachmentFocus;
    std::string to_insert;
    if (acecode::tui::should_fold_to_placeholder(normalized)) {
        const int id = state.next_paste_id++;
        state.pasted_texts[id] = normalized;
        const int n = acecode::tui::count_newlines(normalized);
        to_insert = acecode::tui::format_placeholder(id, n);
    } else {
        to_insert = normalized;
    }
    acecode::insert_at_cursor(
        state.input_text, state.input_cursor, to_insert);
    state.history_index = -1;
}

bool can_accept_clipboard_paste_locked(const TuiState& state) {
    if (state.ask_pending) {
        return state.ask_session && state.ask_session->snapshot().editing_custom;
    }
    return !state.confirm_pending &&
           !state.rewind_picker_active &&
           !state.resume_picker_active &&
           !state.model_picker_open &&
           !state.mode_picker_open;
}

bool paste_clipboard_text(TuiInputContext& ctx) {
    auto& state = ctx.state;
    auto& screen = ctx.screen;
    auto& cmd_registry = ctx.commands;
    const std::string cwd = ctx.turn.cwd();
    {
        std::lock_guard<std::mutex> lk(state.mu);
        if (!can_accept_clipboard_paste_locked(state)) {
            return true;
        }
    }

    auto clipboard = ctx.clipboard.read_text();

    {
        std::lock_guard<std::mutex> lk(state.mu);
        if (!can_accept_clipboard_paste_locked(state)) {
            return true;
        }
        if (!clipboard) {
            tui::set_transient_status_line_locked(
                state, tui::clipboard_paste_status_message(clipboard.status));
            screen.post_event(Event::Custom);
            return true;
        }

        std::string normalized =
            acecode::tui::normalize_pasted_text(clipboard.text);
        if (normalized.empty()) {
            tui::set_transient_status_line_locked(
                state,
                tui::clipboard_paste_status_message(
                    acecode::ClipboardTextReadResult::Status::Empty));
            screen.post_event(Event::Custom);
            return true;
        }

        if (state.ask_pending && state.ask_session &&
            state.ask_session->snapshot().editing_custom) {
            const auto ask_effects = state.ask_session->dispatch({
                tui::AskQuestionEventKind::PasteText,
                -1,
                0,
                normalized});
            tui::dispatch_ask_session_effects_locked(state, ask_effects);
        } else {
            insert_pasted_text_at_cursor_locked(state, normalized);
            refresh_input_suggestions(state, cmd_registry, cwd);
        }
    }
    screen.post_event(Event::Custom);
    return true;
}

bool paste_clipboard_image(TuiInputContext& ctx) {
    auto& state = ctx.state;
    auto& screen = ctx.screen;
    auto& session_manager = ctx.session;
    const auto& working_dir = ctx.working_dir;
    {
        std::lock_guard<std::mutex> lk(state.mu);
        if (!can_accept_clipboard_paste_locked(state) ||
            state.input_mode != InputMode::Normal) {
            return true;
        }
    }

    auto clipboard = ctx.clipboard.read_image();

    {
        std::lock_guard<std::mutex> lk(state.mu);
        if (!can_accept_clipboard_paste_locked(state)) {
            return true;
        }
        if (!clipboard) {
            tui::set_transient_status_line_locked(
                state, tui::clipboard_image_status_message(clipboard.status));
            screen.post_event(Event::Custom);
            return true;
        }
    }

    const std::string session_id = session_manager.ensure_active_session_id();
    const std::string project_dir = SessionStorage::get_project_dir(working_dir);
    std::string error;
    auto record = save_attachment(
        project_dir,
        session_id,
        "clipboard.png",
        clipboard.mime_type.empty() ? "image/png" : clipboard.mime_type,
        clipboard.bytes,
        &error);

    {
        std::lock_guard<std::mutex> lk(state.mu);
        if (!record.has_value()) {
            tui::set_transient_status_line_locked(
                state,
                error.empty() ? "Clipboard image save failed" : error);
            screen.post_event(Event::Custom);
            return true;
        }
        cancel_ctrl_c_exit_locked(state);
        state.pending_attachments.push_back(attachment_to_json(*record));
        acecode::tui::clamp_pending_attachment_focus(
            state.pending_attachment_focus,
            state.pending_attachments.size());
        tui::set_transient_status_line_locked(
            state,
            "Attached image: " + record->name);
    }
    screen.post_event(Event::Custom);
    return true;
}

InputDisposition handle_bracketed_paste(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    auto& screen = ctx.screen;
    auto& cmd_registry = ctx.commands;
    // 多行粘贴折叠（fix-multiline-paste-input change）：把所有事件先喂进
    // bracketed paste 状态机。begin marker 进入 in-paste，期间所有事件
    // （含 Return / Tab / 字符 / 内嵌 CSI bytes）都聚合到 buffer 而不下发到
    // 正常 Return / 字符 / 删除 / 方向键 handler；end marker 触发 normalize
    // 后或 inline 插入或折叠成 [Pasted text #N +M lines]。空 paste 直接返回。
    {
        std::unique_lock<std::mutex> lk(state.mu);
        acecode::tui::PasteFeedResult pr = event.is_character()
            ? state.paste_accumulator.feed_character(event.character())
            : state.paste_accumulator.feed_special(event.input());
        if (pr.just_completed && !pr.completed_text.empty()) {
            // 题目页吞掉粘贴,避免文本漏进隐藏 composer;Other 输入态
            // 才把归一化文本插进自定义答案缓冲
            // (add-tui-ask-overlay-mouse-select)。
            if (can_accept_clipboard_paste_locked(state)) {
                if (state.ask_pending && state.ask_session &&
                    state.ask_session->snapshot().editing_custom) {
                    const auto ask_effects = state.ask_session->dispatch({
                        tui::AskQuestionEventKind::PasteText,
                        -1,
                        0,
                        pr.completed_text});
                    tui::dispatch_ask_session_effects_locked(state, ask_effects);
                } else {
                    insert_pasted_text_at_cursor_locked(state, pr.completed_text);
                    refresh_input_suggestions(
                        state, cmd_registry, ctx.turn.cwd());
                }
                lk.unlock();
                screen.post_event(Event::Custom);
            }
        }
        if (pr.consume) {
            return InputDisposition::Consumed;
        }
    }
    return InputDisposition::Continue;
}

}
