#include "tui/app/animation_ticker.hpp"
#include "tui/app/update_check_task.hpp"
#include "tui/app/copilot_auth_task.hpp"
#include "tui/app/mcp_status_binding.hpp"
#include "tui/app/model_pool_monitor_subscription.hpp"
#include "tui/app/inbound_submit_registration.hpp"
#include "tui/app/full_screen_surfaces.hpp"
#include "session_host/auto_title_runner.hpp"
#include "tui/app/tui_notification_binding.hpp"
#include "tui/app/tui_agent_bridge.hpp"
#include "tui/app/tui_overlay_gate.hpp"
#include "tui/app/tui_turn_lifecycle.hpp"
#include "tui/app/tui_submitter.hpp"
#include "tui/model/user_turn_state.hpp"
#include "tui/app/tui_event_router.hpp"
#include "skills/skill_usage_store.hpp"
#include "config/mcp_config.hpp"
#include "tool/mcp_scope.hpp"
#include "environment/bootstrap.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <array>
#include <sstream>
#include <string_view>
#include <random>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <shellapi.h> // CommandLineToArgvW(-p 模式的 UTF-8 argv 重建)
#else
#include <termios.h>
#include <unistd.h>
#endif

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "version.hpp"
#include "config/config.hpp"
#include "headless/headless_options.hpp"
#include "headless/headless_runner.hpp"
#include "cli/channels_cli.hpp"
#include "utils/encoding.hpp"
#include "platform/power_inhibitor.hpp"
#include "network/proxy_resolver.hpp"
#include "remote_control/remote_control_service.hpp"
#include "provider/provider_factory.hpp"
#include "provider/copilot_provider.hpp"
#include "provider/model_context_resolver.hpp"
#include "provider/model_pool_status.hpp"
#include "provider/models_dev_registry.hpp"
#include "provider/model_resolver.hpp"
#include "provider/cwd_model_override.hpp"
#include "session_host/apply_model_to_session.hpp"
#include "tool/tool_executor.hpp"
#include "tool/bash_tool.hpp"
#include "tool/builtin_tool_registry.hpp"
#include "tool/tool_rewrites.hpp"
#include "security/audit_log.hpp"
#include "tool/file_read_tool.hpp"
#include "tool/file_write_tool.hpp"
#include "tool/file_edit_tool.hpp"
#include "tool/grep_tool.hpp"
#include "tool/glob_tool.hpp"
#include "tool/task_complete_tool.hpp"
#include "tool/goal_tool.hpp"
#include "tool/mcp_manager.hpp"
#include "tool/mcp_startup_coordination.hpp"
#include "session_host/tools/thread_tools.hpp"
#include "tool/workspace_tools.hpp"
#include "tool/skills_tool.hpp"
#include "tool/skill_view_tool.hpp"
#include "tool/memory_read_tool.hpp"
#include "tool/memory_write_tool.hpp"
#include "tool/ask_user_question_tool.hpp"
#include "tui/confirm_question.hpp"
#include "session/permission_prompter.hpp"
#include "skills/skill_init.hpp"
#include "skills/skill_registry.hpp"
#include "tui/commands/skill_commands.hpp"
#include "skills/default_skill_seeder.hpp"
#include "tui/commands/opencode_command_registry.hpp"
#include "hooks/hook_config.hpp"
#include "hooks/hook_manager.hpp"
#include "agent/hook_bridge/hook_events.hpp"
#include "hooks/hook_payload.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_registry.hpp"
#include "lsp/lsp_service.hpp"
#include "tool/web_search/runtime.hpp"
#include "tool/web_search/backend_router.hpp"
#include "tool/web_search/region_detector.hpp"
#include "tool/web_search/web_search_tool.hpp"
#include "utils/logger.hpp"
#include "utils/paths.hpp"
#include "permissions/permissions.hpp"
#include "agent/agent_loop.hpp"
#include "tui/tui_ask_channel.hpp"
#include "session_host/thread_service.hpp"
#include "cli/interactive_options.hpp"
#include "cli/process_environment.hpp"
#include "tui/app/startup_environment.hpp"
#include "tui/app/startup_worktree.hpp"
#include "tui/app/tui_runtime_init.hpp"
#include "tui/model/initial_state.hpp"
#include "tui/term/terminal_control.hpp"
#include "tui/commands/command_bootstrap.hpp"
#include "permissions/default_rules.hpp"
#include "cli/command_dispatch.hpp"
#include "cli/pre_tui_commands.hpp"
#include "skills/default_skill_startup.hpp"
#include "cli/configure/configure.hpp"
#include "daemon/cli.hpp"
#include "web/remote_web_proxy.hpp"
#ifdef _WIN32
#  include "daemon/service_win.hpp"
#endif
#include "upgrade/apply.hpp"
#include "upgrade/check.hpp"
#include "upgrade/manifest.hpp"
#include "upgrade/upgrade.hpp"
#include "tui/commands/command_registry.hpp"
#include "tui/commands/builtin_commands.hpp"
#include "agent/compaction/compact.hpp"
#include "tui/commands/resume_state_sync.hpp"
#include "platform/native_ui/notifications.hpp"
#include "session/token_tracker.hpp"
#include "tui/markdown/markdown_formatter.hpp"
#include "session/session_manager.hpp"
#include "session_host/session_auto_title.hpp"
#include "llm/text_preamble_tags.hpp"
#include "session_host/session_registry.hpp"
#include "tui/resume/session_resume_restore.hpp"
#include "worktree/worktree_core.hpp"
#include "worktree/worktree_manager.hpp"
#include "tui/chat_line_measure.hpp"
#include "tui/chat_file_link.hpp"
#include "tui/chat_message_spacing.hpp"
#include "tui/chat_scroll.hpp"
#include "tui/chat/chat_viewport.hpp"
#include "tui/render/frame_geometry.hpp"
#include "tui/chat_render_window.hpp"
#include "tui/diff_view.hpp"
#include "tui/unclipped_reflect.hpp"
#include "tui/paste_handler.hpp"
#include "tui/pending_attachment_selection.hpp"
#include "tui/ask_question_layout.hpp"
#include "tui/ask_question_adapter.hpp"
#include "tui/ask_question_panel.hpp"
#include "tui/ask_question_view.hpp"
#include "tui/picker_scroll.hpp"
#include "tui/render_mode_factory.hpp"
#include "platform/terminal/terminal_capability.hpp"
#include "platform/open_url.hpp"
#include "utils/state_file.hpp"
#include "tui/slash_command_usage.hpp"
#include "tui/slash_dropdown.hpp"
#include "tui/path_reference_dropdown.hpp"
#include "tui/path_reference_input.hpp"
#include "tui/text_truncation.hpp"
#include "tui/thick_vscroll_bar.hpp"
#include "tui/redraw_pacer.hpp"
#include "tui/input/input_trace.hpp"
#include "tui/thinking_animation.hpp"
#include "tui/compact_animation.hpp"
#include "tui/compact_notice_row.hpp"
#include "tui/thinking_heartbeat.hpp"
#include "tui/model_retry_status.hpp"
#include "tui/tool_progress.hpp"
#include "tui/tool_result_fold.hpp"
#include "tui/tool_row_format.hpp"
#include "tui/tool_row_presentation.hpp"
#include "tui/text_style.hpp"
#include "tui/theme_palette.hpp"
#include "tui/settings/management_center.hpp"
#include "tui/settings/settings_center.hpp"
#include "tui/model/thinking_phrases.hpp"
#include "tui/model/mcp_sidebar_model.hpp"
#include "tui/render/text_cells.hpp"
#include "tui/render/status_chips.hpp"
#include "tui/render/regular_sidebar_view.hpp"
#include "tui/render/tool_row_view.hpp"
#include "tui/overlays/ask_session_projection.hpp"
#include "tui/overlays/ask_question_input.hpp"
#include "tui/overlays/confirm_overlay_input.hpp"
#include "tui/overlays/rewind_picker_input.hpp"
#include "tui/overlays/completion_dropdown_input.hpp"
#include "tui/overlays/list_picker_input.hpp"
#include "tui/input/chat_keys.hpp"
#include "tui/input/tui_input_context.hpp"
#include "tui/composer/paste.hpp"
#include "tui/composer/suggestions.hpp"
#include "tui/composer/pending_attachment.hpp"
#include "tui/composer/input_component.hpp"
#include "tui/composer/edit_keys.hpp"
#include "tui/composer/submit.hpp"
#include "tui/composer/clipboard_keys.hpp"
#include "tui/model/input_state.hpp"
#include "tui/app/tui_command_context_factory.hpp"
#include "tui/app/tui_clipboard.hpp"
#include "tui/render/transcript_view.hpp"
#include "tui/render/overlay_views.hpp"
#include "tui/render/frame_renderer.hpp"
#include "tui/app/tui_screen_host.hpp"
#include "tui/render/ask_question_style.hpp"
#include "tui/render/header_view.hpp"
#include "tui/render/activity_indicator_view.hpp"
#include "tui/render/picker_views.hpp"
#include "tui/render/prompt_status_view.hpp"
#include "tui/render/link_hover_tooltip.hpp"
#include "tui/composer/input_wrap_view.hpp"
#include "tui/vertical_scroll.hpp"
#include "platform/terminal/terminal_theme_detect.hpp"
#include "tui/subagent_host.hpp"
#include "tui/terminal_key_event.hpp"
#include "session_host/tools/spawn_subagent_tool.hpp"
#include "tui/todo_checklist_view.hpp"
#include "tui/input_history_navigation.hpp"
#include "tui/message_render_cache.hpp"
#include "tui/ctrl_c_exit.hpp"
#include "utils/base64.hpp"
#include "platform/clipboard.hpp"
#include "tui/drag_scroll.hpp"
#include "platform/terminal/terminal_title.hpp"
#include "tui/text_input_ops.hpp"
#include "session/attachment_store.hpp"
#include "session/session_storage.hpp"
#include "session/compact_notice.hpp"
#include "history/input_history_recorder.hpp"
#include "session/composer_attachments.hpp"
#include "tui/model/status_line.hpp"
#include "tui/model/turn_lifecycle_rules.hpp"
#include "tui/chat/message_render_revision.hpp"
#include "tui/render/frame_layout.hpp"
#include "tui/overlays/rewind_picker_model.hpp"
#include "workspace/workspace_registry.hpp"

#include <cstdio>
#include <limits>

using ftxui::Event;
using ftxui::Renderer;
using ftxui::ScreenInteractive;
namespace Container = ftxui::Container;
using namespace acecode;

namespace acecode { struct TuiState; }

namespace {

// Semantic theme colors for the question panel. The panel resolves its own
// colors instead of inheriting a decorator from the composing container: a
// container-wide `color(...)` was what turned the whole chat area blue.






}  // namespace

// ---- Get current working directory ----



// ---- Reset terminal cursor visibility on exit ----


// ---- Session finalization on exit ----
static SessionManager* g_session_manager = nullptr;

// Active FTXUI screen, used by signal / console-ctrl handlers to trigger a
// graceful Loop exit. Must be cleared before ScreenInteractive is destroyed
// so handlers can't dereference a dead object.
// App::Exit() is thread-safe (posts a task internally — see app.cpp:1063).
static std::atomic<ftxui::ScreenInteractive*> g_active_screen{nullptr};

static void finalize_session_atexit() {
    if (g_session_manager) {
        g_session_manager->finalize();
        auto sid = g_session_manager->current_session_id();
        if (!sid.empty()) {
            std::cerr << "\nacecode: session " << sid
                      << " saved. Resume with: acecode --resume " << sid << std::endl;
        }
    }
    // Best-effort: hand the window title back to the parent shell. Not
    // strictly async-signal-safe but consistent with the existing finalize
    // path which already uses iostreams.
    clear_terminal_title();
}

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    // Normal Ctrl+C is handled through stdin once ENABLE_PROCESSED_INPUT is
    // cleared after FTXUI installs its terminal mode. This handler remains as
    // a fallback for hosts that still deliver CTRL_C_EVENT, and for
    // Ctrl+Break/close events where graceful shutdown is more important than
    // prompting.
    auto* s = g_active_screen.load(std::memory_order_acquire);
    if (ctrl_type == CTRL_C_EVENT) {
        if (s) {
            s->PostEvent(ftxui::Event::CtrlC);
            return TRUE;
        }
        finalize_session_atexit();
        return FALSE;
    }
    if (ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        // Break / 关窗:明确退出意图,直接走 FTXUI 优雅退出 —— Loop 返回后
        // ScreenInteractive 析构跑 on_exit_functions,把 alt-screen /
        // mouse tracking / Windows console mode 还原回去。返回 FALSE 会让
        // 默认 handler TerminateProcess(),终端会留在 mouse-tracking 开
        // 的状态,父 shell 收到鼠标事件原样喷成乱码字节。
        if (s) {
            s->Exit();
            return TRUE;
        }
        finalize_session_atexit();
        return FALSE;
    }
    return FALSE;
}

static void prepare_windows_ctrl_c_handling_after_ftxui_install() {
    auto stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD in_mode = 0;
    if (stdin_handle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(stdin_handle, &in_mode)) {
        // FTXUI preserves ENABLE_PROCESSED_INPUT from the original console
        // mode. When it stays enabled, Windows turns Ctrl+C into a console
        // control event instead of a stdin byte, and FTXUI's SIGINT handler can
        // exit before our Event::CtrlC branch can handle double-press exit.
        SetConsoleMode(stdin_handle, in_mode & ~ENABLE_PROCESSED_INPUT);
    }

    // Re-register after FTXUI's Loop/PreMain installs its own signal handling
    // so this handler gets first chance at CTRL_BREAK_EVENT / CTRL_CLOSE_EVENT
    // and any CTRL_C_EVENT fallback.
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
}
#else
#include <csignal>
static void signal_handler(int /*sig*/) {
    // FTXUI overrides SIGINT/SIGTERM during Loop() (app.cpp:575) so this only
    // fires before Loop starts or after it returns — terminal isn't in the
    // raw/alt-screen state yet, so _exit is safe.
    finalize_session_atexit();
    _exit(1);
}
#endif

// ---- Shared TUI state ----
// TuiState is shared by the TUI model and its views.
using acecode::TuiState;









// 把粘贴文本放到光标处；长文本折叠成可展开占位符。

// 问答自定义编辑状态由 ask_session snapshot() 提供，普通 composer 不参与。


// 从系统剪贴板粘贴文本，并刷新输入建议。

// 从系统剪贴板粘贴图片，保存成当前会话附件。

// 处理待发送附件列表的聚焦、移动和删除。

// 权限确认框独占键盘，负责把用户选择唤醒给 agent 线程。
// respond_remote 非空且当前请求来自子会话(confirm_remote_session_id 非空)
// 时,用户选择改经它路由回子会话(SubagentHost::respond_permission),
// 本地 confirm_cv 无等待者,不 notify。

// 清空 rewind picker 的临时状态，取消和提交后都复用。

// 根据目标是否支持代码恢复，生成 rewind 的二级选项。

// 提交 rewind 模式，执行回调并恢复普通输入状态。

// /rewind 是两级菜单；/fork 复用目标页并直接提交 conversation-only。

// slash 下拉只负责补全；Enter 补全后继续交给普通提交逻辑。


static int run_interactive_app(const InteractiveCliOptions& cli,
                               const std::string& argv0_dir);









// 实现已抽到 src/skills/skill_init.{hpp,cpp},供 TUI 与 daemon(src/daemon/worker.cpp)
// 共用。下方所有 `initialize_skill_registry(...)` 调用站点经 `using namespace acecode`
// 解析到 `acecode::initialize_skill_registry`。











static void run_tui_loop(ftxui::ScreenInteractive& screen,
                         const ftxui::Component& renderer) {
#ifdef _WIN32
    // FTXUI applies its console mode inside Loop()/PreMain, so queue this
    // adjustment as the first loop task. From then on Ctrl+C arrives as
    // Event::CtrlC instead of being consumed by the Windows console layer.
    screen.Post(prepare_windows_ctrl_c_handling_after_ftxui_install);
#endif
    // Enable bracketed paste so the terminal frames pasted text with
    // ESC[200~ … ESC[201~. FTXUI exposes those markers as Event::Special, and
    // the paste accumulator in the CatchEvent body intercepts them before
    // normal Return / character handlers run — preventing pasted newlines
    // from accidentally submitting partial prompts.
    tui::flush_terminal_input_buffer();
    tui::write_terminal_control_sequence(acecode::tui::kBracketedPasteEnableSeq);
    screen.Loop(renderer);
    tui::write_terminal_control_sequence(acecode::tui::kBracketedPasteDisableSeq);
    tui::flush_terminal_input_buffer();
}

static void shutdown_after_tui_loop(TuiState& state,
                                    AgentLoop& agent_loop,
                                    McpManager& mcp_manager,
                                    std::atomic<bool>& agent_aborting,
                                    tui::AnimationTicker& animation,
                                    tui::CopilotAuthTask& auth_task,
                                    tui::UpdateCheckTask& update_check,
                                    SessionManager& session_manager,
                                    const AppConfig& config,
                                    tui::TuiNotificationBinding& notifications,
                                    tui::InboundSubmitRegistration& inbound_submit) {
    notifications.shutdown();
    g_active_screen.store(nullptr, std::memory_order_release);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
#endif

    animation.request_stop();

    // 先停 remote-control listener:teardown 期间不再接受 IM 入站,也避免
    // 静态析构阶段才停 Crow/asio 的顺序问题。
    inbound_submit.stop();

    // Graceful shutdown: abort agent, unblock confirm_cv / ask_cv, then join worker
    agent_aborting = true;
    agent_loop.abort();
    {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.confirm_pending) {
            state.confirm_pending = false;
            state.confirm_result = PermissionResult::Deny;
            state.confirm_cv.notify_one();
        }
        if (state.ask_pending) {
            // AskUserQuestion 工具线程也在 wait,通知它醒来走拒绝分支。
            // agent_loop.abort() 已经置 abort_flag,工具的 wait 谓词因此成立。
            state.ask_pending = false;
            state.ask_completion_override.reset();
            state.ask_cv.notify_one();
        }
        // 子代理排队占用者(等 overlay 空闲的工具线程)也要放行:它们的
        // 等待谓词含各自的 abort_flag,notify 后自行走拒绝分支。
        state.remote_confirm_queue.clear();
        state.overlay_cv.notify_all();
    }
    agent_loop.shutdown();
    acecode::release_process_session_power(tui::kTuiMainPowerSessionId);

    // Tear down MCP child processes after the agent worker has stopped, so no
    // in-flight tool calls can race with the clients being destroyed.
    mcp_manager.shutdown();

    // LSP server 子进程同理:agent worker 已停,不会再有工具调用打进来。
    lsp::shutdown();

    // Abort and join any in-progress compact thread
    state.compact_abort_requested.store(true);
    if (state.compact_thread.joinable()) {
        state.compact_thread.join();
    }

    animation.join();

    auth_task.join();

    update_check.join();

    tui::finalize_session_worktree_on_exit(session_manager);

    // Finalize session before exit
    session_manager.finalize();
    session_manager.cleanup_old_sessions(config.max_sessions);

    // Print session ID so user knows how to resume
    auto exit_sid = session_manager.current_session_id();
    g_session_manager = nullptr;
    if (!exit_sid.empty()) {
        std::cerr << "\nacecode: session " << exit_sid
                  << " saved. Resume with: acecode --resume " << exit_sid << std::endl;
    }
}

// --worktree 启动引导:在主仓 .acecode/worktrees/ 下创建(或复用)worktree,
// 把进程 cwd 与 working_dir 都切进去。失败打 stderr 并返回 false(直接退出)。
// 语义对齐 Claude Code setup.ts 的 --worktree 分支:--worktree 意味着
// worktree 就是本次会话的项目根 —— 日志 / workspace 注册 / 会话存储全部
// 落在 worktree 里(EnterWorktree 工具中途进入的 throwaway worktree 则
// 相反,项目身份保持在原目录)。

// 准备 TUI 启动的最外层环境：终端、标题、cwd、日志和工作区索引。
// --worktree 时先切进 worktree,再以 worktree 为根初始化其余环境。

// 读取旧版 hook 配置；出错只记日志，不阻塞 TUI 启动。

// 加载配置后刷新 hook registry，并初始化启动期全局运行时。

// 创建当前 provider，并把模型上下文窗口写回运行时配置。

// 注册工具、skills、memory、MCP；这些是 AgentLoop 的工具底座。

// 填充 TUI 初始状态：侧栏、模型状态、输入历史和启动提示。

// 初始化主题并决定 FTXUI 渲染模式。


int main(int argc, char* argv[]) try {
    acecode::cli::configure_process_environment();

    if (auto exit_code = acecode::cli::dispatch_non_tui_command(argc, argv)) {
        return *exit_code;
    }

    std::string argv0_dir = acecode::cli::get_executable_dir_from_argv(argc, argv);
    InteractiveCliOptions cli = parse_interactive_cli_options(argc, argv);

    if (auto exit_code = acecode::cli::run_pre_tui_command(cli, argv0_dir)) {
        return *exit_code;
    }

    return run_interactive_app(cli, argv0_dir);
} catch (const McpConfigError& error) {
    // Startup cannot recover an unvalidated scope without a last-good copy.
    // Return the same structured diagnostic as managed configuration writes.
    std::cerr << error.what() << std::endl;
    return 1;
}

static int run_interactive_app(const InteractiveCliOptions& cli,
                               const std::string& argv0_dir) {
    const bool dangerous_mode = cli.dangerous_mode;
    const bool resume_latest = cli.resume_latest;
    const bool resume_picker_on_startup =
        cli.resume_picker_on_startup && !cli.direct_resume_requested();
    const bool force_alt_screen = cli.force_alt_screen;
    const std::string resume_session_id = cli.resume_session_id;

    std::string working_dir;
    WorktreeSessionInfo startup_worktree;
    std::string startup_worktree_banner;
    if (!tui::initialize_tui_startup_environment(working_dir, cli, startup_worktree,
                                            startup_worktree_banner)) {
        return 1;
    }
    HookConfig hook_config = tui::load_tui_hook_config();
    HookManager hook_manager(std::move(hook_config));
    {
        auto payload = build_startup_before_model_load_payload(working_dir);
        hook_manager.dispatch(kHookEventStartupBeforeModelLoad, payload, working_dir);
    }

    AppConfig config =
        tui::load_tui_config_and_runtime(hook_manager, working_dir, argv0_dir);

    // --question-policy 覆盖(add-ask-question-policy):非法值 fail fast;
    // 合法值只写运行时 CLI 字段,save_config 永不落盘。
    if (!cli.question_policy_error.empty()) {
        std::cerr << "[acecode] " << cli.question_policy_error << std::endl;
        return 1;
    }
    if (!cli.question_policy.empty()) {
        config.agent_loop.question_policy_cli = cli.question_policy;
        config.agent_loop.question_timeout_seconds_cli = cli.question_timeout_seconds;
    }

    auto cwd_override = load_cwd_model_override(working_dir);
    SessionModelBinding model_binding;
    ModelProfile initial_model_profile = tui::initialize_tui_provider_runtime(
        config, working_dir, cwd_override, model_binding, hook_manager);
    auto provider_accessor = [&model_binding]() {
        return model_binding.provider_snapshot();
    };

    // 「工具重写」与 daemon 共用同一份 <data_dir>/tool-rewrites.json,
    // 必须先于 register_tool 发布(见 src/tool/tool_rewrites.hpp)。
    tool_rewrites::load_and_apply(get_acecode_dir());
    // 安全审计存储(openspec add-security-center):TUI 的审批决策同样入账,
    // 在 Desktop 的安全中心里查看。
    security::audit_log().configure(get_acecode_dir());

    ToolExecutor tools;
    SkillRegistry skill_registry;
    MemoryRegistry memory_registry;
    McpManager mcp_manager;
    MemoryConfig runtime_memory_cfg = tui::initialize_tui_tools_and_registries(
        tools, skill_registry, memory_registry, mcp_manager, config, working_dir);
    const std::string workspace_projects_dir = path_to_utf8(
        path_from_utf8(get_acecode_dir()) / "projects");
    desktop::WorkspaceRegistry workspace_registry;
    workspace_registry.scan(workspace_projects_dir);
    auto workspace_tool_deps = std::make_shared<WorkspaceToolDeps>();
    workspace_tool_deps->registry = &workspace_registry;
    workspace_tool_deps->projects_dir = workspace_projects_dir;
    register_workspace_tools(tools, workspace_tool_deps);

    // Skill usage / dormancy state shared by the TUI session and any daemon
    // surface. Best-effort: a read/write failure never blocks the session.
    auto skill_usage_store = std::make_shared<SkillUsageStore>(
        get_acecode_dir() + "/.skill_usage_state.json");

    TuiState state;
    tui::initialize_tui_state_before_screen(state, config, working_dir, dangerous_mode,
                                       mcp_manager, provider_accessor());
    state.skill_usage_store = skill_usage_store;
    state.slash_command_usage_counts = read_tui_slash_command_usage();
    if (!startup_worktree_banner.empty()) {
        state.conversation.push_back({"system", startup_worktree_banner, false});
    }

    // Version and working directory strings for TUI header
    std::string version_str = "acecode v" ACECODE_VERSION;
    std::string cwd_display = working_dir;

    // Animation tick for Thinking... indicator
    std::atomic<int> anim_tick{0};
    tui::ChatViewport viewport;
    tui::FrameGeometry geometry;
    auto& chat_box = viewport.chat_box;
    auto& message_layout_boxes = viewport.message_layout_boxes;
    auto& message_layout_valid = viewport.message_layout_valid;
    auto& message_layout_revisions = viewport.message_layout_revisions;
    auto& message_layout_widths = viewport.message_layout_widths;
    auto& message_line_measures = viewport.message_line_measures;
    auto& message_line_counts = viewport.message_line_counts;
    auto& message_spacer_rows_after = viewport.message_spacer_rows_after;
    auto& message_line_count_width = viewport.message_line_count_width;
    auto& scrollbar_box = geometry.scrollbar_box;
    auto& ask_question_frame = geometry.ask_question_frame;
    auto& sidebar_content_box = geometry.sidebar_content_box;
    auto& sidebar_viewport_box = geometry.sidebar_viewport_box;
    auto& sidebar_scrollbar_box = geometry.sidebar_scrollbar_box;
    auto& input_hit_layout = geometry.input_hit_layout;
    auto& message_boxes = geometry.message_boxes;
    auto& path_reference_boxes = geometry.path_reference_boxes;
    auto& chat_link_regions = geometry.chat_link_regions;

    acecode::TerminalCapabilities term_caps;
    bool conhost_compat_layout = false;
    auto render_mode = tui::initialize_tui_render_mode(
        config, force_alt_screen, term_caps, conhost_compat_layout);
    tui::maybe_add_legacy_terminal_hint(state, config, term_caps, render_mode,
                                   force_alt_screen);

    tui::TuiScreenHost screen_host(render_mode, config.tui);
    auto& screen = screen_host.screen();
    const bool hover_supported = screen_host.hover_supported();
    auto redraw_pacer = screen_host.redraw_pacer();
    auto& last_keyboard_input_at_ms = screen_host.last_keyboard_input_at_ms();
    // Publish for the Windows console-ctrl handler so Ctrl+C can trigger a
    // graceful Loop exit instead of letting the default handler kill us.
    g_active_screen.store(&screen, std::memory_order_release);
    screen.ForceHandleCtrlC(false);
    // mouse-selection-copy: register a no-op SelectionChange callback so
    // FTXUI enables live selection tracking. The right-click branch in the
    // CatchEvent handler reads screen.GetSelection() on demand; we don't
    // need per-change work here yet. Future: hook "auto-clear on new drag".
    screen.SelectionChange([]{});
    auto update_check = std::make_unique<tui::UpdateCheckTask>(config, state, screen_host);

    // AskUserQuestion 两端同一个工厂:工具逻辑只有一份,传输由
    // ToolContext::ask_user_questions 注入(见 agent_loop 里的 set_ask_question_channel
    // 接线)。无需等 state/screen 就绪,但保留在这里以免和下面的 MCP
    // 启动顺序拉开。
    tools.register_tool(create_ask_user_question_tool_async(
        config.ask.max_questions, config.ask.max_options));
    auto mcp_status = std::make_unique<tui::McpStatusBinding>(mcp_manager, tools, state, screen_host);

    std::atomic<bool> mcp_first_turn_wait_done{false};
    SessionManager session_manager;
    std::unique_ptr<AutoTitleRunner> auto_title_runner;
    tui::TuiSubmitter input_turn(state, screen_host, config, model_binding,
        session_manager, mcp_manager, mcp_first_turn_wait_done, auto_title_runner);

    // ---- Copilot auth flow (background thread) ----
    std::atomic<bool> auth_done{false};
    auto auth_task = std::make_unique<tui::CopilotAuthTask>(
        provider_accessor, state, screen_host, auth_done);

    // ---- Token tracking ----
    TokenTracker token_tracker;
    state.token_status = token_tracker.format_status(config.context_window);
    state.token_percent = token_tracker.context_percent(config.context_window);
    state.cache_hit_percent = token_tracker.cache_hit_percent();

    // ---- Agent callbacks ----
    std::atomic<bool> agent_aborting{false};  // shared abort flag for confirm_cv
    // Set only by the authoritative final assistant on_message callback. Delta
    // text is intentionally excluded so a failed/aborted partial stream cannot
    // be mistaken for a completed task notification.
    tui::TurnObservation turn_observation;
    std::unique_ptr<tui::TuiNotificationBinding> notification_binding;
    tui::TuiOverlayGate overlay_gate(state, screen_host, agent_aborting);
    tui::TuiTurnLifecycle turn_lifecycle(state, screen_host, viewport, input_turn,
        session_manager, config, turn_observation, auto_title_runner, notification_binding);
    tui::TuiAgentBridge agent_bridge(state, screen_host, viewport, token_tracker,
        config, turn_observation);
    auto callbacks = agent_bridge.initial_callbacks();
    callbacks.on_tool_confirm = overlay_gate.confirm_callback();

    PermissionManager permissions;
    configure_tui_default_permissions(permissions, dangerous_mode, config.default_permission_mode);

    AgentLoop agent_loop(provider_accessor, tools, callbacks, working_dir, permissions);
    input_turn.attach(agent_loop);
    overlay_gate.attach(agent_loop);
    turn_lifecycle.attach(agent_loop);
    agent_loop.set_tool_capability_policy(
        mcp_scope_policy(&config, working_dir, std::nullopt, &mcp_manager, &tools));
    // TUI 侧的 AskUserQuestion 传输。接上之后任何工具都能向用户提问
    // (不只是 AskUserQuestion 工具本身),且行为与 daemon 路径同源。
    agent_loop.set_ask_question_channel(
        [&state, &screen](const nlohmann::json& questions_payload,
                          const std::atomic<bool>* abort_flag,
                          int timeout_seconds,
                          const std::string& origin_label) {
            return acecode::tui::ask_via_tui_overlay(
                state, screen, questions_payload, abort_flag,
                timeout_seconds, origin_label);
        });
    agent_loop.set_context_window(config.context_window);
    agent_loop.set_task_suggestion_compact_threshold(
        config.task_suggestion_compact_threshold);
    agent_loop.set_no_model_config_prompt(
        u8"请先配置大模型服务。TUI 可运行 acecode configure 或使用 /model add 添加模型。");
    agent_loop.set_agent_loop_config(config.agent_loop);
    agent_loop.set_sandbox_config(config.sandbox);
    agent_loop.set_hook_manager(&hook_manager);
    agent_loop.set_skill_registry(&skill_registry);
    agent_loop.set_skill_usage_store(skill_usage_store.get());
    agent_loop.set_skill_idle_days(config.skills.idle_days);
    agent_loop.set_memory_registry(&memory_registry);
    agent_loop.set_memory_config(&runtime_memory_cfg);
    agent_loop.set_project_instructions_config(&config.project_instructions);
    agent_loop.set_custom_instructions_config(&config.custom_instructions);
    agent_loop.set_git_context_config(&config.git_context);

    agent_loop.set_callbacks(callbacks);

    auto model_pool_monitor = std::make_unique<tui::ModelPoolMonitorSubscription>(
        model_binding, config, agent_loop, screen_host.post_target());

    // ---- Session manager ----
    {
        auto p = provider_accessor();
        const std::string provider_name = p ? p->name() : std::string{};
        const std::string provider_model = p ? p->model() : std::string{};
        session_manager.start_session(working_dir,
                                      provider_name,
                                      provider_model,
                                      std::string{},
                                      initial_model_profile.name);
        session_manager.set_permission_mode(
            PermissionManager::mode_name(permissions.mode()),
            /*persist_immediately=*/false);
        if (permissions.mode() == PermissionMode::Plan) {
            session_manager.set_pre_plan_permission_mode(
                PermissionManager::mode_name(permissions.pre_plan_mode()),
                /*persist_immediately=*/false);
        }
        // --worktree 启动:worktree 会话状态挂到 SessionManager 并随 meta
        // 持久化,ExitWorktree 工具与退出时的收尾逻辑都以它为准。
        if (startup_worktree.active()) {
            session_manager.set_active_worktree(startup_worktree);
        }
    }
    agent_loop.set_session_manager(&session_manager);

    auto_title_runner = std::make_unique<AutoTitleRunner>(
        config, session_manager, agent_loop, turn_lifecycle.title_applied_callback());
    callbacks.on_turn_finished = turn_lifecycle.title_finished_callback();
    agent_loop.set_callbacks(callbacks);
    auto submit_tui_input = input_turn.callback();

    // ---- 子代理宿主(spawn_subagent / wait_subagent)----
    // SessionRegistry / LocalSessionClient 不依赖 web 层,TUI 进程内直接
    // 实例化;子会话与主会话共享 ToolExecutor(深度限制阻止孙代理)。
    acecode::tui::SubagentHost::Deps subagent_host_deps;
    {
        SessionRegistryDeps rd;
        rd.provider_accessor = provider_accessor;
        rd.tools = &tools;
        rd.cwd = working_dir;
        rd.config = &config;
        rd.mcp_manager = &mcp_manager;
        rd.skill_registry = &skill_registry;
        rd.memory_registry = &memory_registry;
        rd.memory_cfg = &runtime_memory_cfg;
        rd.project_instructions_cfg = &config.project_instructions;
        rd.custom_instructions_cfg = &config.custom_instructions;
        rd.hook_manager = &hook_manager;
        rd.template_permissions = &permissions;
        rd.power_guard = &acecode::process_power_guard();
        subagent_host_deps.registry_deps = std::move(rd);
    }
    subagent_host_deps.parent_session_id = [&session_manager]() {
        return session_manager.current_session_id();
    };
    subagent_host_deps.publish_tasks =
        [&state, &screen](std::vector<acecode::tui::SubagentTaskSnapshot> tasks) {
            {
                std::lock_guard<std::mutex> lk(state.mu);
                state.subagent_tasks.clear();
                state.subagent_tasks.reserve(tasks.size());
                for (auto& t : tasks) {
                    state.subagent_tasks.push_back(
                        {t.id, t.title, t.prompt, t.started});
                }
            }
            screen.PostEvent(Event::Custom);
        };
    subagent_host_deps.on_permission_request =
        [&state, &screen](const std::string& session_id,
                          const std::string& task_title,
                          nlohmann::json payload) {
            TuiState::RemoteConfirmRequest req;
            req.session_id = session_id;
            req.request_id = payload.value("request_id", std::string{});
            req.tool = payload.value("tool", std::string{});
            if (payload.contains("args")) {
                req.args_preview = payload["args"].is_string()
                    ? payload["args"].get<std::string>()
                    : payload["args"].dump(2);
            }
            req.origin_label = "[subagent] " +
                (task_title.empty() ? session_id : task_title);
            {
                std::lock_guard<std::mutex> lk(state.mu);
                state.remote_confirm_queue.push_back(std::move(req));
            }
            // Custom 事件驱动 CatchEvent 入口的泵,在 overlay 空闲时弹出展示。
            screen.PostEvent(Event::Custom);
        };
    acecode::tui::SubagentHost subagent_host(std::move(subagent_host_deps));
    {
        auto subagent_deps = std::make_shared<SubagentToolDeps>();
        subagent_deps->registry = &subagent_host.registry();
        subagent_deps->client = &subagent_host.client();
        subagent_deps->config = &config;
        subagent_deps->fallback_permissions = &permissions;
        subagent_deps->on_spawn =
            [&subagent_host](const std::string& child_id,
                             const std::string& prompt) {
                subagent_host.on_spawned(child_id, prompt);
            };
        tools.register_tool(create_spawn_subagent_tool(subagent_deps));
        tools.register_tool(create_wait_subagent_tool(subagent_deps));
        auto thread_deps = std::make_shared<ThreadToolDeps>();
        thread_deps->service = std::make_shared<ThreadService>(
            ThreadService::Deps{
                &subagent_host.registry(), &subagent_host.client()});
        register_codex_thread_tools(tools, std::move(thread_deps));
    }

    // Register session finalization for clean shutdown
    g_session_manager = &session_manager;
    std::atexit(finalize_session_atexit);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    // ---- Handle --resume ----
    bool resumed_session_success = false;
    if (resume_latest || !resume_session_id.empty()) {
        std::string target_id = resume_session_id;
        std::string resumed_title;
        if (resume_latest) {
            auto sessions = session_manager.list_sessions();
            if (!sessions.empty()) {
                target_id = sessions.front().id;
                resumed_title = sessions.front().title;
            }
        } else if (!target_id.empty()) {
            // 通过 SessionManager 读取 canonical meta,避免在 TUI 启动路径里
            // 直接拼存储路径。
            auto meta = session_manager.load_session_meta(target_id);
            resumed_title = meta.title;
        }
        if (!target_id.empty()) {
            const bool canonical_exists = session_manager.has_session_file(target_id);
            // 把 session meta 喂给 resolver,让 resume 真正还原 provider+model。
            // resolve_effective_model 会优先用 (meta.provider, meta.model) 从
            // saved_models 找匹配 entry;找不到则构造 ad-hoc entry,name 以
            // "(session:..." 开头,触发我们后面的系统消息提示。
            SessionMeta resumed_meta = session_manager.load_session_meta(target_id);
            std::optional<SessionModelState> resumed_model_state;
            if (auto deleted_state = deleted_model_state_from_meta(config, resumed_meta)) {
                resumed_model_state = deleted_state;
            } else if (!resumed_meta.provider.empty() && !resumed_meta.model.empty()) {
                ModelProfile resumed_entry = resolve_effective_model(
                    config, cwd_override, std::optional<SessionMeta>{resumed_meta});
                ApplyModelDeps deps;
                deps.model_binding = &model_binding;
                deps.sm = &session_manager;
                deps.loop = &agent_loop;
                deps.cfg = &config;
                try {
                    auto result = apply_model_to_session(resumed_entry, deps);
                    config.context_window = result.state.context_window;
                    resumed_model_state = result.state;
                } catch (const std::exception& e) {
                    state.conversation.push_back({"system",
                        std::string("⚠ Resume model switch failed: ") + e.what(), false});
                }
                if (resumed_entry.name.rfind("(session:", 0) == 0) {
                    state.conversation.push_back({"system",
                        "⚠ Resumed with ad-hoc model entry (session recorded " +
                        resumed_meta.provider + "/" + resumed_meta.model +
                        ", not in saved_models). Use /model --default <name> to pick a permanent one.",
                        false});
                }
            }

            auto messages = session_manager.resume_session(target_id);
            const std::string resume_error = session_manager.last_error();
            if (!resume_error.empty()) {
                state.conversation.push_back({"system", resume_error, false});
            } else if (!canonical_exists && session_manager.has_incompatible_session_data(target_id)) {
                state.conversation.push_back({"system",
                    "Session " + target_id + " uses an old PID-suffixed data format that is no longer supported. Delete the old project session data under ~/.acecode/projects and start a new session.", false});
            } else if (!canonical_exists) {
                state.conversation.push_back({"system", "Session " + target_id + " not found.", false});
            } else {
                acecode::append_resumed_session_messages(messages, state, agent_loop, tools);
                state.todos = session_manager.current_todos();
                // worktree 会话恢复:meta 记录的 worktree 还在就把会话 cwd
                // 切回去;目录已被外部删除则清状态,避免 ExitWorktree 之后
                // 操作幽灵路径。
                {
                    const WorktreeSessionInfo resumed_worktree =
                        session_manager.active_worktree();
                    if (resumed_worktree.active()) {
                        std::error_code wt_ec;
                        if (std::filesystem::exists(
                                path_from_utf8(resumed_worktree.worktree_path), wt_ec)) {
                            agent_loop.set_cwd(resumed_worktree.worktree_path);
                            std::filesystem::current_path(
                                path_from_utf8(resumed_worktree.worktree_path), wt_ec);
                            state.conversation.push_back({"system",
                                "Resumed inside worktree " +
                                    resumed_worktree.worktree_path +
                                    (resumed_worktree.worktree_branch.empty()
                                         ? std::string{}
                                         : " (branch " + resumed_worktree.worktree_branch + ")"),
                                false});
                        } else {
                            session_manager.clear_active_worktree();
                            state.conversation.push_back({"system",
                                "Worktree " + resumed_worktree.worktree_path +
                                    " no longer exists; resumed in " + working_dir + ".",
                                false});
                        }
                    }
                }
                const PermissionMode resumed_mode =
                    parse_tui_permission_mode_name(resumed_meta.permission_mode);
                if (resumed_mode == PermissionMode::Plan) {
                    permissions.set_mode(parse_tui_permission_mode_name(
                        resumed_meta.pre_plan_permission_mode.empty()
                            ? std::string{"default"}
                            : resumed_meta.pre_plan_permission_mode));
                    permissions.set_mode(PermissionMode::Plan);
                } else {
                    permissions.set_mode(resumed_mode);
                }
                permissions.clear_session_allows();
                session_manager.set_permission_mode(
                    PermissionManager::mode_name(permissions.mode()),
                    /*persist_immediately=*/false);
                if (permissions.mode() == PermissionMode::Plan) {
                    session_manager.set_pre_plan_permission_mode(
                        PermissionManager::mode_name(permissions.pre_plan_mode()),
                        /*persist_immediately=*/false);
                }
                token_tracker.restore(resumed_meta.last_token_usage,
                                      resumed_meta.session_token_usage);
                sync_tui_resume_runtime_state(state, config, token_tracker,
                                              resumed_model_state);
                state.conversation.push_back({"system",
                    "Resumed session " + target_id + " (" + std::to_string(messages.size()) + " messages)", false});
                resumed_session_success = true;
                agent_loop.publish_current_goal_state();
                agent_loop.maybe_continue_goal();
                if (!resumed_title.empty()) {
                    set_terminal_title(resumed_title);
                    state.current_session_title = resumed_title;
                }
            }
        } else {
            if (session_manager.has_incompatible_session_data()) {
                state.conversation.push_back({"system",
                    "No canonical sessions found. Old PID-suffixed session data in this project is no longer supported; delete the old project session data under ~/.acecode/projects and start a new session.", false});
            } else {
                state.conversation.push_back({"system", "No previous sessions found to resume.", false});
            }
        }
    }
    agent_loop.dispatch_session_start_hook(
        resumed_session_success ? std::string{"resume"} : std::string{"startup"});
    if (resumed_session_success) {
        agent_loop.dispatch_session_title_changed_hook(
            session_manager.current_title(),
            "resume",
            session_manager.current_title_source());
    }

    // Slash command registry
    CommandRegistry cmd_registry;
    tui::register_slash_commands(cmd_registry, skill_registry, config, working_dir);

    std::unique_ptr<tui::IFullScreenSurfaces> full_screen_surfaces;
    tui::TuiCommandContextFactory input_commands(state, agent_loop, model_binding,
        config, token_tracker, permissions, screen_host, session_manager, mcp_manager,
        tools, skill_registry, memory_registry, cmd_registry, working_dir, input_turn,
        &subagent_host, full_screen_surfaces);

    notification_binding = std::make_unique<tui::TuiNotificationBinding>(
        config, state, screen_host, viewport, session_manager, input_commands);

    if (resume_picker_on_startup) {
        auto cmd_ctx = input_commands.make(false);
        cmd_registry.dispatch("/resume", cmd_ctx);
    }

    // --- Tool progress callbacks (streaming-tool-progress change) ---
    agent_bridge.install_progress_callbacks(callbacks);





    // Now that agent_loop exists, update on_busy_changed to drain pending queue
    callbacks.on_busy_changed = turn_lifecycle.busy_callback();
    agent_loop.set_callbacks(callbacks);

    // remote-control 入站:IM 桥经 loopback HTTP 送来的文本走输入框同款提交
    // 路径(busy 排队 / 空闲直接 submit),气泡渲染与会话持久化与手输完全一致。
    // 该回调由 RC listener 的 Crow worker 线程调用,锁内逻辑与 Enter 提交分支
    // 保持一字不差。
    auto inbound_submit = std::make_unique<tui::InboundSubmitRegistration>(
        state, screen_host, viewport, input_turn);

    auto animation = std::make_unique<tui::AnimationTicker>(
        state, screen_host, viewport, anim_tick, conhost_compat_layout);

    // ---- Input handling ----
    // Custom input component using paragraph-like flexbox for auto-wrapping.
    // Uses FTXUI's focusCursorBlock so the terminal cursor tracks the caret,
    // which lets the terminal emulator position the IME composition window.
    // NOTE: Renderer must take (bool) to be Focusable. This ensures
    // component_active=true on the element, so the input's cursor always
    // wins focus priority over message_view's | focus (which has
    // component_active=false and cursor_shape=Hidden).
    auto input_renderer = tui::make_composer_input(state, input_hit_layout);

    tui::TuiClipboard input_clipboard;
    tui::TuiInputContext input_context{
        state, screen_host, viewport, geometry, cmd_registry, input_turn, input_commands,
        input_clipboard, config, permissions, session_manager, auth_done, working_dir,
        last_keyboard_input_at_ms,
        [&subagent_host](const std::string& sid, const std::string& rid, PermissionResult result) {
            subagent_host.respond_permission(sid, rid, permission_result_choice_name(result));
        },
    };

    tui::TuiEventRouter event_router{input_context};
    auto input_with_esc = event_router.wrap(input_renderer);

    tui::TuiFrameRenderer frame_renderer{
        state, screen_host, version_str, cwd_display, viewport, geometry, anim_tick,
        input_with_esc, permissions, dangerous_mode, conhost_compat_layout, hover_supported,
    };
    auto chat_renderer = Renderer(
        input_with_esc,
        [&frame_renderer, &screen, redraw_pacer] {
            const auto frame_ticket = redraw_pacer->begin_frame(
                tui::monotonic_milliseconds());
            auto frame = frame_renderer.render();
            // FTXUI closures do not invalidate the frame. This one runs on the
            // next loop turn, after the current Draw/TerminalFlush completed,
            // and therefore measures conservative end-to-end frame latency.
            screen.Post([redraw_pacer, frame_ticket] {
                redraw_pacer->complete_frame(
                    frame_ticket, tui::monotonic_milliseconds());
            });
            return frame;
        });

    auto surfaces = std::make_unique<tui::FullScreenSurfaces>(
        state, screen_host, config, session_manager, agent_loop, subagent_host,
        skill_registry, cmd_registry, mcp_manager, tools, hook_manager, skill_usage_store.get(),
        working_dir, chat_renderer, input_with_esc);
    auto root_surface = surfaces->component();
    full_screen_surfaces = std::move(surfaces);
    run_tui_loop(screen, root_surface);
    // Stop background publishers in the original order before shutting down
    // the turn producer; their callbacks now carry revocable owner references.
    model_pool_monitor->stop();
    auto_title_runner->stop();
    shutdown_after_tui_loop(state, agent_loop, mcp_manager,
                            agent_aborting, *animation, *auth_task,
                            *update_check,
                            session_manager, config, *notification_binding, *inbound_submit);

    return 0;
}
