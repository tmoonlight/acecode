#pragma once

#include "utils/logger.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace acecode {

struct SessionReadMetrics {
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    std::uint64_t records = 0;

    SessionReadMetrics operator-(const SessionReadMetrics& prior) const {
        return {bytes - prior.bytes, files - prior.files, records - prior.records};
    }
};

// Each synchronous operation observes its own thread; nested operations count
// toward their enclosing request without sharing mutable state across workers.
inline SessionReadMetrics& session_read_metrics() {
    static thread_local SessionReadMetrics metrics;
    return metrics;
}

// A per-thread synchronous test observer; no callback is stored in a session.
inline std::function<void()>& session_read_observer() {
    static thread_local std::function<void()> observer;
    return observer;
}

class ScopedSessionReadObserver {
public:
    explicit ScopedSessionReadObserver(std::function<void()> observer)
        : previous_(std::exchange(session_read_observer(), std::move(observer))) {}
    ~ScopedSessionReadObserver() { session_read_observer() = std::move(previous_); }
    ScopedSessionReadObserver(const ScopedSessionReadObserver&) = delete;
    ScopedSessionReadObserver& operator=(const ScopedSessionReadObserver&) = delete;
private:
    std::function<void()> previous_;
};

class SessionLoadTimer {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using Sink = std::function<void(LogLevel, const nlohmann::json&)>;

    explicit SessionLoadTimer(std::string operation, std::string identifier = {},
                              Clock clock = [] { return std::chrono::steady_clock::now(); },
                              Sink sink = {})
        : operation_(std::move(operation)), identifier_(std::move(identifier)),
          clock_(std::move(clock)), sink_(std::move(sink)), started_(clock_()),
          baseline_(session_read_metrics()) {}

    ~SessionLoadTimer() noexcept {
        try {
            const auto elapsed = std::chrono::duration<double, std::milli>(clock_() - started_).count();
            const auto counts = session_read_metrics() - baseline_;
            const nlohmann::json payload{
                {"operation", operation_}, {"identifier", identifier_},
                {"elapsed_ms", elapsed}, {"read_bytes", counts.bytes},
                {"opened_files", counts.files}, {"parsed_records", counts.records},
            };
            const auto level = elapsed > 500.0 ? LogLevel::Warn : LogLevel::Dbg;
            if (sink_) sink_(level, payload);
            else Logger::instance().log(level, __FILE__, __LINE__, "[session-load] " + payload.dump());
        } catch (...) {
            // Instrumentation must never turn a successful request into a failure.
        }
    }

    SessionLoadTimer(const SessionLoadTimer&) = delete;
    SessionLoadTimer& operator=(const SessionLoadTimer&) = delete;

private:
    std::string operation_;
    std::string identifier_;
    Clock clock_;
    Sink sink_;
    std::chrono::steady_clock::time_point started_;
    SessionReadMetrics baseline_;
};

} // namespace acecode
