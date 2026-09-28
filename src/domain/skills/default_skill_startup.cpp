#include "default_skill_startup.hpp"
#include "default_skill_seeder.hpp"
#include "utils/paths.hpp"
#include "utils/logger.hpp"
#include <filesystem>

namespace acecode {

void reconcile_default_skills_on_startup(const std::string& argv0_dir) {
    auto result = acecode::reconcile_default_global_skills_on_startup(
        std::filesystem::path(acecode::get_acecode_dir()),
        argv0_dir);
    if (!result.attempted) return;

    size_t installed = 0;
    size_t updated = 0;
    size_t unchanged = 0;
    size_t preserved = 0;
    size_t errors = 0;
    const auto count_outcomes = [&](const auto& outcomes) {
        for (const auto& outcome : outcomes) {
            if (outcome.result == "installed") ++installed;
            else if (outcome.result == "updated") ++updated;
            else if (outcome.result == "unchanged") ++unchanged;
            else if (outcome.result == "preserved_user_modified") ++preserved;
            else ++errors;
        }
    };
    count_outcomes(result.outcomes);
    count_outcomes(result.expert_outcomes);
    count_outcomes(result.hook_outcomes);
    if (!result.error.empty()) {
        LOG_WARN("[seed] Default resource reconciliation issue: " + result.error);
    }
    LOG_INFO("[seed] Default resource reconciliation attempted: version=" +
             result.bundle_version + " installed=" + std::to_string(installed) +
             " updated=" + std::to_string(updated) +
             " unchanged=" + std::to_string(unchanged) +
             " preserved=" + std::to_string(preserved) +
             " errors=" + std::to_string(errors));
}


} // namespace acecode
