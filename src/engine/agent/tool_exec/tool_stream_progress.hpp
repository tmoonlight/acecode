#pragma once

#include "utils/stream_processing.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace acecode::agent {

class ToolStreamProgress {
public:
    using Clock = std::chrono::steady_clock;
    struct Snapshot {
        std::vector<std::string> tail_lines;
        std::string current_partial;
        size_t total_bytes = 0;
        int total_lines = 0;
        bool should_emit = false;
    };

    // Only state accounting is locked. Observers consume the returned value
    // after unlock, including reentrant or concurrently delivering tool streams.
    Snapshot append(const std::string& chunk, Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mu_);
        feed_line_state(chunk, current_line_, tail_lines_, total_lines_);
        total_bytes_ += chunk.size();
        Snapshot snapshot{{tail_lines_.begin(), tail_lines_.end()},
                          current_line_, total_bytes_, total_lines_};
        snapshot.should_emit = last_emit_at_.time_since_epoch().count() == 0 ||
            now - last_emit_at_ >= std::chrono::milliseconds(500);
        if (snapshot.should_emit) last_emit_at_ = now;
        return snapshot;
    }

private:
    std::mutex mu_;
    std::string current_line_;
    std::deque<std::string> tail_lines_;
    int total_lines_ = 0;
    size_t total_bytes_ = 0;
    Clock::time_point last_emit_at_{};
};

} // namespace acecode::agent
