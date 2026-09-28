#include "terminal_runtime.hpp"

#include "utils/logger.hpp"

namespace acecode::environment {

TerminalRuntime& TerminalRuntime::instance() {
    static TerminalRuntime runtime;
    return runtime;
}

TerminalRuntime& terminal() {
    return TerminalRuntime::instance();
}

void TerminalRuntime::publish(const TerminalResolution& resolution) {
    std::lock_guard<std::mutex> lk(mu_);
    last_ = resolution;
}

TerminalResolution TerminalRuntime::reresolve(const ConsoleConfig& console) {
    return reresolve(console, default_shell_probe(), default_launch_probe());
}

TerminalResolution TerminalRuntime::reresolve(const ConsoleConfig& console,
                                              const ShellProbe& probe,
                                              const LaunchProbe& launch) {
    // 探测在锁外跑(会 spawn 进程),只有发布这一步加锁。
    TerminalResolution result = resolve_terminal(console, probe, launch);
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_ = result;
    }
    if (result.resolved.usable) {
        LOG_INFO("[terminal] resolved default terminal: id=" + result.resolved.id +
                 " family=" + terminal_family_name(result.resolved.family) +
                 " program=" + result.resolved.program +
                 (result.resolved.fallback_reason.empty()
                      ? std::string{}
                      : " fallback=" + result.resolved.fallback_reason));
    }
    return result;
}

std::optional<ResolvedTerminal> TerminalRuntime::current() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!last_ || !last_->resolved.usable) return std::nullopt;
    return last_->resolved;
}

std::optional<TerminalResolution> TerminalRuntime::last() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_;
}

void TerminalRuntime::set_for_test(const ResolvedTerminal& terminal) {
    std::lock_guard<std::mutex> lk(mu_);
    TerminalResolution r;
    r.resolved = terminal;
    r.configured_id = terminal.id;
    last_ = r;
}

void TerminalRuntime::reset_for_test() {
    std::lock_guard<std::mutex> lk(mu_);
    last_.reset();
}

}  // namespace acecode::environment
