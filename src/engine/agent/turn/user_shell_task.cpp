#include "agent/agent_loop.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/turn/busy_cycle.hpp"
#include "agent/tool_exec/tool_stream_progress.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "agent/tool_exec/tool_batch_types.hpp"
#include "computer_use/runtime.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_runtime.hpp"
#include "llm/tool_protocol_names.hpp"
#include "pa/pa_context_budget.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/task_suggestion_store.hpp"
#include "session/token_tracker.hpp"
#include "session/turn_timing.hpp"
#include "tool/ask_user_question_tool.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "utils/text.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

using agent::detail::build_session_scratch_dir;

void AgentLoop::run_shell(std::string command) {
    abort_signal_.clear();
    busy_ = true;

    LOG_WARN("user_initiated_shell: " + log_truncate(command, 200));

    if (session_manager_) {
        session_manager_->record_trajectory_event(
            "busy_changed", {{"busy", true}});
    }
    if (callbacks_.on_busy_changed) {
        callbacks_.on_busy_changed(true);
    }

    agent::BusyCycleScope finish([this] {
        record_terminal_trajectory_events(
            {{"busy", false}}, nlohmann::json::object());
        if (callbacks_.on_busy_changed) {
            callbacks_.on_busy_changed(false);
        }
        busy_ = false;
        events_.emit(SessionEventKind::BusyChanged, nlohmann::json{{"busy", false}});
        events_.emit(SessionEventKind::Done, nlohmann::json::object());
    });

    // Surface the invocation in the TUI using the usual tool_call styling so
    // the user sees a clear "-> bash command" line followed by its result.
    nlohmann::json args = {{"command", command}};
    std::string args_json = args.dump();
    bool hook_denied_shell = false;
    if (hook_manager_) {
        auto fields = build_hook_common_fields(kCodexHookEventPreToolUse);
        auto payload = build_tool_hook_payload(fields, "bash", args);
        auto outcome = dispatch_codex_hook(kCodexHookEventPreToolUse, "bash", payload);
        apply_hook_side_effects(outcome);
        if (outcome.updated_input.has_value()) {
            const auto& updated = *outcome.updated_input;
            if (updated.is_object() && updated.contains("command") &&
                updated["command"].is_string()) {
                command = updated["command"].get<std::string>();
                args = {{"command", command}};
                args_json = args.dump();
            }
        }
        if (outcome.denied || outcome.blocked) {
            hook_denied_shell = true;
        }
    }
    dispatch_message("tool_call", "[Tool: bash] " + args_json, true);

    ToolResult result{"[Error] bash tool not registered", false};
    if (hook_denied_shell) {
        result = ToolResult{"[Hook denied tool execution]", false};
    } else if (tools_.has_tool("bash")) {
        // Same progress plumbing as the agent-driven bash path.
        std::string cmd_preview = command;
        cmd_preview = truncate_utf8_prefix(cmd_preview, 60);

        auto prog = std::make_shared<agent::ToolStreamProgress>();

        ToolContext tool_ctx;
        tool_ctx.cwd = boundary_->cwd();
        tool_ctx.write_root = write_root();
        tool_ctx.abort_flag = &abort_signal_.flag_for_legacy_api();
        tool_ctx.session_manager = session_manager_;
        tool_ctx.scratch_dir = build_session_scratch_dir(boundary_->cwd(), session_manager_);
        if (callbacks_.on_tool_progress_update) {
            auto update_cb = callbacks_.on_tool_progress_update;
            tool_ctx.stream = [prog, update_cb](const std::string& chunk) {
                const auto progress = prog->append(chunk);
                update_cb(progress.tail_lines, progress.current_partial,
                          progress.total_bytes, progress.total_lines);
            };
        }

        struct ProgressGuard {
            std::function<void()> end_cb;
            ~ProgressGuard() { if (end_cb) end_cb(); }
        };
        ProgressGuard guard;
        if (callbacks_.on_tool_progress_start) {
            callbacks_.on_tool_progress_start("bash", cmd_preview, std::string{});
            guard.end_cb = callbacks_.on_tool_progress_end;
        }

        try {
            result = tools_.execute("bash", args_json, tool_ctx);
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("shell exec exception: ") + e.what());
            result = ToolResult{std::string("[Error] ") + e.what(), false};
        }
    } else {
        LOG_WARN("Shell mode invoked but `bash` tool is not registered");
    }
    if (hook_manager_) {
        nlohmann::json response = {
            {"success", result.success},
            {"output", result.output},
        };
        auto fields = build_hook_common_fields(kCodexHookEventPostToolUse);
        auto payload = build_tool_hook_payload(fields, "bash", args, response);
        auto outcome = dispatch_codex_hook(kCodexHookEventPostToolUse, "bash", payload);
        apply_hook_side_effects(outcome);
        if (outcome.replacement_output.has_value()) {
            result.output = *outcome.replacement_output;
            if (outcome.blocked || outcome.continue_false) result.success = false;
        } else if ((outcome.blocked || outcome.continue_false) && !outcome.reason.empty()) {
            result.output = outcome.reason;
            result.success = false;
        }
    }

    // 用户主动 `!cmd` 的输出必须**全量显示**(不折叠、不摘要、不截断)—— 用户
    // 自己输入命令就是为了看完整结果,LLM 工具结果的"摘要 + Ctrl+E 展开"语义
    // 在这里不适用。所以使用一个独立的 TUI 伪角色 `user_shell_output`,渲染分支
    // 走全量路径,与 `tool_result`(LLM 工具结果)区分开。
    // 同样不调 callbacks_.on_tool_result —— 它会把 ToolResult.summary 回填到
    // TuiState::Message,导致渲染走 summary 单行;这正是要避免的。
    dispatch_message("user_shell_output", result.output, true);

    // Persist the two display-side messages so --resume can rehydrate both the
    // chat view and (via the recovery pass in main.cpp) the LLM history.
    // 落盘的 role 仍然是 "tool_result"(伪角色) —— resume 时由 main.cpp
    // 的 shell-mode 配对识别(`is_shell_user && next_is_result`)把它翻译为
    // "user_shell_output"。不写 metadata.tool_summary/tool_hunks,因为
    // user_shell_output 渲染分支不读这些字段,写了也是死字段。
    if (session_manager_) {
        ChatMessage user_msg;
        user_msg.role = "user";
        user_msg.content = "!" + command;
        session_manager_->on_message(user_msg);

        ChatMessage tool_msg;
        tool_msg.role = "tool_result";
        tool_msg.content = result.output;
        session_manager_->on_message(tool_msg);
    }

    // Inject into LLM context for subsequent turns. BashTool currently merges
    // stdout+stderr into `result.output`, so we report it as stdout and leave
    // stderr empty; exit code derives from `success`.
    inject_shell_turn(command, result.output, "", result.success ? 0 : 1);

    finish.finish();
}

} // namespace acecode
