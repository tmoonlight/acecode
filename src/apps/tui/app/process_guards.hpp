#pragma once
#include "utils/lifetime_token.hpp"
namespace acecode { class SessionManager; }
namespace ftxui { class ScreenInteractive; }
namespace acecode::tui {
class IScreenPort;
class TerminalRestoreGuard {
public:
    static void install();
};
class SessionFinalizeRegistration {
public:
    explicit SessionFinalizeRegistration(SessionManager& session);
    ~SessionFinalizeRegistration() { release(); }
    SessionFinalizeRegistration(const SessionFinalizeRegistration&) = delete;
    SessionFinalizeRegistration& operator=(const SessionFinalizeRegistration&) = delete;
    void release();
private:
    SessionManager* session_;  // Borrowed until release, owner outlives registration.
};
class ActiveScreenRegistration {
public:
    explicit ActiveScreenRegistration(ftxui::ScreenInteractive& screen);
    ~ActiveScreenRegistration() { release(); }
    ActiveScreenRegistration(const ActiveScreenRegistration&) = delete;
    ActiveScreenRegistration& operator=(const ActiveScreenRegistration&) = delete;
    void release();
private:
    ftxui::ScreenInteractive* screen_;  // Borrowed; registration precedes screen destruction.
};
class ConsoleCtrlHandlerRegistration {
public:
    ConsoleCtrlHandlerRegistration();
    ~ConsoleCtrlHandlerRegistration();
    ConsoleCtrlHandlerRegistration(const ConsoleCtrlHandlerRegistration&) = delete;
    ConsoleCtrlHandlerRegistration& operator=(const ConsoleCtrlHandlerRegistration&) = delete;
    void post_ftxui_setup(IScreenPort& screen);
    void release();
private:
    void prepare_after_ftxui_install();
    bool registered_ = false;
    LifetimeToken lifetime_;
};
}
