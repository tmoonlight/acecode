#pragma once

#include "config/config.hpp"
#include "prompt/system_prompt.hpp"
#include <atomic>
#include <optional>
#include <string>

namespace acecode::agent {

// Worker-owned pins. Only invalidate_git is cross-thread; the worker consumes
// that atomic flag before collecting a new snapshot.
class PromptContextCache {
public:
    void reset_on_cwd_change();
    void invalidate_git() { git_stale_.store(true); }
    void prepare_git(const std::string& cwd, const GitContextConfig* config, bool emergency);
    std::string cached_git() const { return git_snapshot_.value_or(std::string{}); }
    std::string skills(const PromptContextBlock& block);
    std::string session(const PromptContextBlock& block);
private:
    std::optional<std::string> git_snapshot_;
    std::atomic<bool> git_stale_{false};
    std::string skill_key_, skill_content_;
    std::string session_key_, session_content_;
};

} // namespace acecode::agent
