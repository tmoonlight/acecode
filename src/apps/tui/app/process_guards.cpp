#include "tui/app/process_guards.hpp"
#include "tui/screen_port.hpp"
#include "tui/term/terminal_control.hpp"
#include "session/session_manager.hpp"
#include "platform/terminal/terminal_title.hpp"
#include <ftxui/component/screen_interactive.hpp>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif
namespace acecode::tui {
static std::atomic<SessionManager*> g_session_manager{nullptr};
static std::atomic<ftxui::ScreenInteractive*> g_active_screen{nullptr};
static std::once_flag session_atexit_once;
static std::once_flag terminal_atexit_once;
static void finalize_session_atexit() {
    if (auto* session = g_session_manager.load(std::memory_order_acquire)) {
        session->finalize();
        auto sid = session->current_session_id();
        if (!sid.empty()) {
            std::cerr << "\nacecode: session " << sid
                      << " saved. Resume with: acecode --resume " << sid << std::endl;
        }
    }
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

#else
static void signal_handler(int /*sig*/) {
    // FTXUI overrides SIGINT/SIGTERM during Loop() (app.cpp:575) so this only
    // fires before Loop starts or after it returns — terminal isn't in the
    // raw/alt-screen state yet, so _exit is safe.
    finalize_session_atexit();
    _exit(1);
}
#endif

void TerminalRestoreGuard::install() {
    std::call_once(terminal_atexit_once, [] { std::atexit(reset_cursor); });
}
SessionFinalizeRegistration::SessionFinalizeRegistration(SessionManager& session)
    : session_(&session) {
    g_session_manager.store(session_, std::memory_order_release);
    std::call_once(session_atexit_once, [] { std::atexit(finalize_session_atexit); });
}
void SessionFinalizeRegistration::release() {
    if (!session_) return;
    auto* expected = session_;
    g_session_manager.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    session_ = nullptr;
}
ActiveScreenRegistration::ActiveScreenRegistration(ftxui::ScreenInteractive& screen)
    : screen_(&screen) { g_active_screen.store(screen_, std::memory_order_release); }
void ActiveScreenRegistration::release() {
    if (!screen_) return;
    auto* expected = screen_;
    g_active_screen.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    screen_ = nullptr;
}
ConsoleCtrlHandlerRegistration::ConsoleCtrlHandlerRegistration() {
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif
    registered_ = true;
}
ConsoleCtrlHandlerRegistration::~ConsoleCtrlHandlerRegistration() {
    lifetime_.revoke();
    release();
}
void ConsoleCtrlHandlerRegistration::release() {
    if (!registered_) return;
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
#endif
    registered_ = false;
}
void ConsoleCtrlHandlerRegistration::post_ftxui_setup(IScreenPort& screen) {
#ifdef _WIN32
    screen.post_task([ref = lifetime_.ref(*this)] {
        ref.with([](ConsoleCtrlHandlerRegistration& owner) { owner.prepare_after_ftxui_install(); });
    });
#endif
}
void ConsoleCtrlHandlerRegistration::prepare_after_ftxui_install() {
#ifdef _WIN32
    if (!registered_) return;
    auto stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD in_mode = 0;
    if (stdin_handle != INVALID_HANDLE_VALUE && GetConsoleMode(stdin_handle, &in_mode))
        SetConsoleMode(stdin_handle, in_mode & ~ENABLE_PROCESSED_INPUT);
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif
}
}
