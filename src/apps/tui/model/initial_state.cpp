#include "initial_state.hpp"
#include "tui/tui_state.hpp"
#include "tui/sidebar_model.hpp"
#include "history/input_history_store.hpp"
#include "session/session_storage.hpp"
#include "tool/mcp_manager.hpp"
#include "tool/mcp_startup_coordination.hpp"
#include "llm/llm_provider.hpp"
#include "utils/state_file.hpp"

namespace acecode::tui {

static void restore_input_history(TuiState& state,
                                  const AppConfig& config,
                                  const std::string& working_dir) {
    // Restore per-working-directory input history. Independent of session files so
    // /clear, resume, or deleting a session never blows away the Up/Down queue.
    // 关闭开关时退化为纯内存历史（旧行为）。
    if (!config.input_history.enabled) {
        return;
    }

    std::string ih_path = InputHistoryStore::file_path(
        SessionStorage::get_project_dir(working_dir));
    state.input_history = InputHistoryStore::load(ih_path);
    // 若历史文件条数超过当前上限（用户把 max_entries 调小），保留最近 N 条。
    int cap = config.input_history.max_entries;
    if (cap > 0 && static_cast<int>(state.input_history.size()) > cap) {
        state.input_history.erase(
            state.input_history.begin(),
            state.input_history.begin() +
                (state.input_history.size() - static_cast<size_t>(cap)));
    }
}

static void add_startup_messages(TuiState& state,
                                 bool dangerous_mode,
                                 const McpManager& mcp_manager) {
    // If dangerous YOLO mode is forced from CLI, show startup warning. Note:
    // this skips permission and path-safety checks but does NOT suppress
    // AskUserQuestion overlays (those are a legitimate LLM-driven request for
    // input).
    if (dangerous_mode) {
        state.conversation.push_back({"system",
            "[DANGEROUS YOLO MODE] Permission and path-safety checks are bypassed. Use with caution. "
            "(AskUserQuestion overlays are still shown when the model needs input.)",
            false});
    }

    const size_t configured = mcp_manager.configured_server_count();
    if (configured > 0) {
        state.conversation.push_back({"system",
            acecode::mcp_background_start_message(configured),
            false});
    }
}

void maybe_add_legacy_terminal_hint(
    TuiState& state,
    const AppConfig& config,
    const acecode::TerminalCapabilities& term_caps,
    acecode::tui::ScreenRenderMode render_mode,
    bool force_alt_screen) {
    // 一次性提示:仅在自动探测命中并切换到 alt-screen 时显示一次。
    // CLI 强制 / config "always" / config "never" / 现代终端都不触发。
    bool show_legacy_hint =
        render_mode == acecode::tui::ScreenRenderMode::AltScreen &&
        !force_alt_screen &&
        config.tui.alt_screen_mode == "auto" &&
        !term_caps.source_label.empty() &&
        !acecode::read_state_flag("legacy_terminal_hint_shown");
    if (show_legacy_hint) {
        std::string hint = "提示: 检测到 " + term_caps.source_label +
            ",已启用全屏渲染避免画面跳动。可在 ~/.acecode/config.json 设置 "
            "\"tui\": {\"alt_screen_mode\": \"never\"} 关闭。";
        state.conversation.push_back({"system", hint, false});
        acecode::write_state_flag("legacy_terminal_hint_shown", true);
    }
}

void initialize_tui_state_before_screen(
    TuiState& state,
    const AppConfig& config,
    const std::string& working_dir,
    bool dangerous_mode,
    const McpManager& mcp_manager,
    const std::shared_ptr<LlmProvider>& provider) {
    tui::set_mcp_sidebar_servers_locked(state, tui::build_mcp_sidebar_servers(mcp_manager));
    state.status_line = provider
        ? "[" + provider->name() + "] model: " + provider->model()
        : "No model configured";
    state.ask_config.min_visible_rows = config.tui.question_min_visible_rows;
    state.ask_config.selection_feedback_ms =
        config.tui.question_selection_feedback_ms;
    restore_input_history(state, config, working_dir);
    add_startup_messages(state, dangerous_mode, mcp_manager);
}


} // namespace acecode::tui
