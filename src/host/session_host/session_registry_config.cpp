#include "session_registry.hpp"
#include "agent/agent_loop.hpp"
#include "config/config.hpp"
#include "skills/skill_init.hpp"
#include "skills/skill_registry.hpp"
#include <shared_mutex>

namespace acecode {
SessionPromptConfig SessionRegistry::prompt_config_snapshot() const {
    if (deps_.prompt_config) return deps_.prompt_config();
    auto copy = [&] {
        SessionPromptConfig snapshot;
        if (deps_.memory_cfg) snapshot.memory = *deps_.memory_cfg;
        if (deps_.project_instructions_cfg) snapshot.project_instructions = *deps_.project_instructions_cfg;
        if (deps_.custom_instructions_cfg) snapshot.custom_instructions = *deps_.custom_instructions_cfg;
        if (deps_.config) snapshot.git_context = deps_.config->git_context;
        return snapshot;
    };
    if (deps_.config_mutex) {
        std::shared_lock<std::shared_mutex> lock(*deps_.config_mutex);
        return copy();
    }
    return copy(); // Immutable headless/test configuration.
}

void SessionRegistry::refresh_skill_policy(const AppConfig& config) {
    struct RefreshTarget {
        std::shared_ptr<SessionEntry> entry;
        std::shared_ptr<SkillRegistry> registry;
        std::string cwd;
        std::vector<std::filesystem::path> roots;
        std::optional<std::vector<std::string>> expert_allowed;
    };

    std::vector<RefreshTarget> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        targets.reserve(entries_.size());
        for (const auto& [id, entry] : entries_) {
            (void)id;
            if (!entry || !entry->skill_registry) continue;
            targets.push_back({
                entry, entry->skill_registry,
                entry->cwd,
                entry->expert_skill_roots,
                entry->expert_skill_allowlist,
            });
        }
    }

    for (auto& target : targets) {
        initialize_skill_registry(
            *target.registry,
            config,
            target.cwd,
            target.roots,
            target.expert_allowed);
        auto snapshot = target.registry->snapshot();
        enqueue_entry_control(target.entry,
            [expected = std::weak_ptr<SkillRegistry>(target.registry), snapshot = std::move(snapshot)]
            (SessionRegistry&, SessionEntry& active) {
                if (active.skill_registry != expected.lock()) return true;
                active.loop->publish_skill_snapshot(snapshot);
                return true;
            });
    }
}

} // namespace acecode
