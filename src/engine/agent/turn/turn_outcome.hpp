#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace acecode::agent {

// Error text is protected by a leaf lock. Outcome is published before busy
// becomes false, so observers see the completed turn's result after the boundary.
class TurnOutcomeRecord {
public:
    void begin();
    void record(const std::string& status);
    void set_error(std::string error);
    std::string error() const;
    bool failed() const { return outcome_.load(std::memory_order_acquire) == Outcome::Error; }

private:
    enum class Outcome { None, Completed, Error, Aborted };
    std::atomic<Outcome> outcome_{Outcome::None};
    mutable std::mutex error_mu_;
    std::string error_;
};

} // namespace acecode::agent
