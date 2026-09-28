#include "tool_call_lifecycle.hpp"
#include "ask_question_binding.hpp"
#include "tool_context_factory.hpp"
#include "tool_invoker.hpp"
#include "tool_result_presenter.hpp"
#include "agent/agent_callbacks.hpp"
#include "agent/hook_bridge/tool_hook_bridge.hpp"
#include "session/session_manager.hpp"
#include "utils/logger.hpp"
#include "utils/text.hpp"
#include "utils/encoding.hpp"
#include "utils/time.hpp"

namespace acecode::agent {
using utils::now_epoch_ms;

ToolCallOutcome ToolCallLifecycle::run(
    ToolBatchState& batch, ToolCall tc, std::size_t tool_index, bool emit_tui_progress) {
    const auto& emit_progress = batch.emit_progress;
    const auto& step_preamble = batch.step_preamble;
    agent::ToolCallOutcome outcome;
    if (auto denied = tool_hooks_.before(hook_manager_, session_manager_, tc, tool_index)) {
        outcome.result = std::move(*denied);
        return outcome;
    }

    std::string exec_path, exec_cmd;
    ToolInvoker::extract_context(tc, exec_path, exec_cmd);

    std::string cmd_preview;
    if (!exec_cmd.empty()) cmd_preview = exec_cmd;
    else if (!exec_path.empty()) cmd_preview = exec_path;
    else cmd_preview = tc.function_name;
    cmd_preview = truncate_utf8_prefix(cmd_preview, 60);

    std::string display_override =
        ToolExecutor::build_tool_call_preview(tc.function_name, tc.function_arguments);

    auto tool_start_tp = std::chrono::steady_clock::now();
    const std::int64_t tool_started_at_ms = now_epoch_ms();
    const int tool_index_int = static_cast<int>(tool_index);
    lifecycle_events_.start(
        tc, tool_index_int, step_preamble, cmd_preview, display_override, tool_started_at_ms);

    // 开启具体进度提示时,发射口把这条换成本批次文案、清掉命令预览。
    emit_progress("tool_running", "正在调用工具 " + tc.function_name,
        cmd_preview, tc.function_name, tc.id, tool_index_int, true);

    ToolContext tool_ctx = contexts_.for_tool();
    questions_.bind(tool_ctx, tc, tool_index_int, emit_progress);
    ToolLifecycleEvents::Stream stream(
        events_, callbacks_, tc, tool_index_int, emit_tui_progress, stream_clock_, tool_start_tp);
    stream.bind(tool_ctx);

    struct ProgressGuard {
        std::function<void()> end_cb;
        ~ProgressGuard() { if (end_cb) end_cb(); }
    };
    ProgressGuard guard;
    if (emit_tui_progress && callbacks_.on_tool_progress_start) {
        callbacks_.on_tool_progress_start(tc.function_name, cmd_preview, std::string{});
        guard.end_cb = callbacks_.on_tool_progress_end;
    }

    ToolResult& result = outcome.result;
    try {
        result = invoker_.invoke(batch, tc, tool_ctx, exec_path, exec_cmd, tool_index, emit_tui_progress);
    } catch (const std::exception& e) {
        LOG_ERROR("Tool lifecycle runner error: " + std::string(e.what()));
        result = ToolResult{"[Error] Tool execution failed: " + std::string(e.what()), false};
    }
    tool_hooks_.after(hook_manager_, session_manager_, tc, result);
    presenter_.materialize_attachments(result);
    mark_workspace_scratch_change(result, tool_ctx);
    if (session_manager_) {
        // Both ToolEnd and the following tool_result Message are sent live.
        // Persist before either can retain a full output in replay/UI state.
        // PostToolUse has already seen its original input; preserve hunks
        // and other structured fields for specialized file-diff rendering.
        if (prepare_tool_result_for_delivery(
                result, tc.function_name, tc.id,
                session_manager_->ensure_tool_results_dir()) && !tc.id.empty()) {
            outcome.delivery_replacement = {tc.id, result.output};
        }
    }
    ensure_tool_summary(
        tc.function_name, tc.function_arguments, result);

    lifecycle_events_.finish(
        tc, tool_index_int, result, tool_start_tp, tool_started_at_ms, outcome);
    return outcome;
}


} // namespace acecode::agent
