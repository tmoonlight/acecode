#include "tui/overlays/ask_session_projection.hpp"
#include "platform/clipboard.hpp"
#include "tui/model/status_line.hpp"
#include <algorithm>

namespace acecode::tui {
int ask_timeout_remaining_seconds(
    const tui::AskQuestionSession& session,
    tui::AskQuestionSession::TimePoint now) {
    const auto deadline = session.timeout_deadline();
    if (!deadline.has_value() || *deadline <= now) return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        *deadline - now).count();
    return static_cast<int>(std::max<std::int64_t>(1, remaining));
}

void project_ask_session_locked(TuiState& state) {
    if (!state.ask_session) return;
    const auto snapshot = state.ask_session->snapshot();
    state.input_text = snapshot.editing_custom ? snapshot.editor.text : std::string{};
    state.input_cursor = snapshot.editing_custom ? snapshot.editor.cursor : 0;
    state.input_selection_anchor = snapshot.editing_custom
        ? snapshot.editor.selection_anchor : std::nullopt;
    state.input_vertical_goal_column.reset();
    if (snapshot.completed || snapshot.cancelled) {
        // AskQuestionCompletion 由 channel 直接从 session 读取；这里仅负责
        // 释放 overlay 占用并唤醒等待线程，不把结构化答案降级成字符串。
        state.ask_pending = false;
        state.ask_cv.notify_all();
        state.overlay_cv.notify_all();
    }
}

void dispatch_ask_session_effects_locked(
    TuiState& state, const std::vector<tui::AskQuestionEffect>& effects) {
    tui::project_ask_session_locked(state);
    for (const auto& effect : effects) {
        if (effect.kind == tui::AskQuestionEffectKind::CopyText ||
            effect.kind == tui::AskQuestionEffectKind::CutText) {
            const auto clipboard_write =
                acecode::write_system_clipboard_text(effect.text);
            const std::string status = clipboard_write
                ? (effect.kind == tui::AskQuestionEffectKind::CutText
                       ? "Cut to clipboard"
                       : "Copied to clipboard")
                : tui::clipboard_copy_status_message(clipboard_write.status);
            tui::set_transient_status_line_locked(state, status);
            if (effect.kind == tui::AskQuestionEffectKind::CutText &&
                clipboard_write && state.ask_session) {
                const auto delete_effects = state.ask_session->dispatch({
                    tui::AskQuestionEventKind::DeleteSelection});
                tui::project_ask_session_locked(state);
                for (const auto& delete_effect : delete_effects) {
                    if (delete_effect.kind == tui::AskQuestionEffectKind::Complete ||
                        delete_effect.kind == tui::AskQuestionEffectKind::Cancel) {
                        tui::project_ask_session_locked(state);
                        break;
                    }
                }
            }
        }
        if (effect.kind == tui::AskQuestionEffectKind::Complete ||
            effect.kind == tui::AskQuestionEffectKind::Cancel) {
            tui::project_ask_session_locked(state);
            break;
        }
    }
}

}
