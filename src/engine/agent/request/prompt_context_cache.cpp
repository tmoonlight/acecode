#include "prompt_context_cache.hpp"
#include "request_context.hpp"
#include "gitinfo/git_context_collector.hpp"
#include "utils/logger.hpp"

namespace acecode::agent {

void PromptContextCache::reset_on_cwd_change() {
    git_snapshot_.reset(); // Skill/session pins follow content keys, not cwd.
}
void PromptContextCache::prepare_git(const std::string& cwd,
                                     const GitContextConfig* config, bool emergency) {
    if (emergency) return;
    if (git_stale_.exchange(false)) git_snapshot_.reset();
    if (git_snapshot_) return;
    const bool enabled = !config || config->enabled;
    const int timeout = config ? config->timeout_ms : gitinfo::kDefaultGitTimeoutMs;
    git_snapshot_ = enabled ? gitinfo::collect_git_status_snapshot(cwd, timeout) : std::string{};
}
std::string PromptContextCache::skills(const PromptContextBlock& block) {
    const bool changed = block.cache_key != skill_key_;
    auto result = detail::cached_context_for_api(block, skill_key_, skill_content_);
    if (changed && !block.warning.empty()) LOG_WARN("[skills] " + block.warning);
    return result;
}
std::string PromptContextCache::session(const PromptContextBlock& block) {
    return detail::cached_context_for_api(block, session_key_, session_content_);
}
bool PromptContextCache::needs_memory_snapshot(const std::string& key) {
    const bool stale = memory_stale_.exchange(false);
    return stale || !memory_ready_ || key != memory_key_;
}
void PromptContextCache::store_memory_snapshot(const std::string& key, PromptContextBlock block) {
    memory_key_ = key;
    memory_block_ = std::move(block);
    memory_ready_ = true;
}
const PromptContextBlock* PromptContextCache::peek_memory_snapshot(const std::string& key) const {
    if (!memory_ready_ || memory_stale_.load() || key != memory_key_) return nullptr;
    return &memory_block_;
}

} // namespace acecode::agent
