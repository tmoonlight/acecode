#include "rewind_picker_model.hpp"

namespace acecode::tui {

void populate_rewind_modes_locked(TuiState& state,
                                         const TuiState::RewindItem& item) {
    state.rewind_modes.clear();
    if (item.can_restore_code) {
        state.rewind_modes.push_back({
            TuiState::RewindRestoreMode::CodeAndConversation,
            "Code and conversation",
            "Restore tracked files, fork the conversation, and prefill the selected prompt."
        });
        state.rewind_modes.push_back({
            TuiState::RewindRestoreMode::ConversationOnly,
            "Conversation only",
            "Fork the conversation and prefill the selected prompt."
        });
        state.rewind_modes.push_back({
            TuiState::RewindRestoreMode::CodeOnly,
            "Code only",
            "Restore tracked files without changing the conversation."
        });
    } else {
        state.rewind_modes.push_back({
            TuiState::RewindRestoreMode::ConversationOnly,
            "Conversation only",
            "Fork the conversation and prefill the selected prompt."
        });
    }
    state.rewind_modes.push_back({
        TuiState::RewindRestoreMode::NeverMind,
        "Never mind",
        "Cancel rewind."
    });
    state.rewind_mode_selected = 0;
}


} // namespace acecode::tui
