#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace acecode { struct ToolResult; }

namespace acecode::agent {

class SafeEditGuard {
public:
    using Clock = std::chrono::steady_clock;
    std::string blocked_path(const std::string& command, bool may_bypass,
                             Clock::time_point now = Clock::now());
    void record_result(const std::string& tool, const std::string& path,
                       const ToolResult& result, Clock::time_point now = Clock::now());
    void check_shell_output(const std::string& command, ToolResult& result) const;
private:
    std::vector<std::string> paths() const;
    mutable std::mutex mu_; // Leaf; no file reads or callbacks under this lock.
    std::map<std::string, Clock::time_point> failures_;
};

} // namespace acecode::agent
