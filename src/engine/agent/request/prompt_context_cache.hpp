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

    // 记忆快照按会话冻结(openspec unify-memory-system D3):key(会话 + 工作区 +
    // 开关)不变且没有被 invalidate_memory 标脏时逐字节复用;压缩 / 线程修复
    // 调 invalidate_memory(跨线程安全),下一次请求从磁盘重建。
    bool needs_memory_snapshot(const std::string& key);
    void store_memory_snapshot(const std::string& key, PromptContextBlock block);
    const PromptContextBlock& memory_snapshot() const { return memory_block_; }
    // 不消费脏标记的只读查看;key 不同或已标脏时返回 nullptr。
    const PromptContextBlock* peek_memory_snapshot(const std::string& key) const;
    void invalidate_memory() { memory_stale_.store(true); }
private:
    std::optional<std::string> git_snapshot_;
    std::atomic<bool> git_stale_{false};
    std::string skill_key_, skill_content_;
    std::string session_key_, session_content_;
    std::atomic<bool> memory_stale_{false};
    bool memory_ready_ = false;
    std::string memory_key_;
    PromptContextBlock memory_block_;
};

} // namespace acecode::agent
