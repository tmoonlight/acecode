#include "safe_edit_guard.hpp"

#include "permissions/shell_write_guard.hpp"
#include "tool/text_file_errors.hpp"
#include "tool/tool_executor.hpp"
#include "utils/text.hpp"

namespace acecode::agent {

std::string SafeEditGuard::blocked_path(
    const std::string& command, bool may_bypass, Clock::time_point now) {
    std::vector<std::string> candidates;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = failures_.begin(); it != failures_.end();) {
            if (now - it->second > std::chrono::minutes(10)) it = failures_.erase(it);
            else { candidates.push_back(it->first); ++it; }
        }
    }
    if (!may_bypass) {
        for (const auto& path : candidates) {
            if (command_mentions_path(command, path)) return path;
        }
    }
    return {};
}

std::vector<std::string> SafeEditGuard::paths() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> result;
    for (const auto& entry : failures_) result.push_back(entry.first);
    return result;
}

void SafeEditGuard::record_result(const std::string& tool, const std::string& path,
                                  const ToolResult& result, Clock::time_point now) {
    if ((tool != "file_edit" && tool != "file_write") || path.empty() || result.success) return;
    const auto lower = utils::ascii_lower(result.output);
    if (lower.find("encoding") == std::string::npos &&
        lower.find("old_string") == std::string::npos &&
        lower.find("round-trip") == std::string::npos) return;
    std::lock_guard<std::mutex> lock(mu_);
    failures_[path] = now;
}

void SafeEditGuard::check_shell_output(const std::string& command, ToolResult& result) const {
    for (const auto& path : paths()) {
        if (!command_mentions_path(command, path)) continue;
        auto check = with_text_file_tool_errors(read_text_file_buffer(path, false));
        if (!check.success) {
            result.success = false;
            if (!result.output.empty() && result.output.back() != '\n') result.output += "\n";
            result.output += "[Error] Post-command encoding sanity check failed for " + path + ": " + check.error;
        }
    }
}

} // namespace acecode::agent
