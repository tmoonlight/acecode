#pragma once

#include <atomic>
#include <chrono>
#include <memory>

namespace acecode::platform {
// A terminal wakeup, backed by a manual-reset event on Windows and a
// nonblocking self-pipe on POSIX. Join all waiters before destroying it.
class TerminationSignal {
public:
    TerminationSignal();
    ~TerminationSignal();
    TerminationSignal(const TerminationSignal&) = delete;
    TerminationSignal& operator=(const TerminationSignal&) = delete;

    void install_process_handlers();
    void request() noexcept;
    bool requested() const noexcept { return requested_.load(); }
    bool wait_for(std::chrono::milliseconds timeout);

    // Also used by the Windows service control callback. Requests preceding
    // handler installation are delivered when the process owner installs.
    static void request_process_termination() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> requested_{false};
};
} // namespace acecode::platform
