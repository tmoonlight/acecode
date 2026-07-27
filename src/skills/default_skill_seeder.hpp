#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

struct DefaultSkillSeed {
    std::string name;
    std::string source_id;
    std::filesystem::path relative_path;
};

struct DefaultExpertSeed {
    std::string name;
    std::string source_id;
    std::filesystem::path relative_path;
};

struct DefaultSkillSeedOutcome {
    std::string name;
    std::string source_id;
    std::string relative_path;
    std::string result;
    std::string message;
    std::string source_tree_sha256;
    std::string installed_tree_sha256;
    bool acecode_owned = false;
};

struct DefaultSkillSeedInstallResult {
    bool attempted = false;
    bool state_written = false;
    bool version_written = false;
    bool downgrade_skipped = false;
    std::string error;
    std::string bundle_version;
    std::string user_version;
    std::filesystem::path seed_skills_dir;
    std::filesystem::path seed_experts_dir;
    std::filesystem::path target_root;
    std::filesystem::path expert_target_root;
    std::filesystem::path state_path;
    std::filesystem::path version_path;
    std::vector<DefaultSkillSeedOutcome> outcomes;
    std::vector<DefaultSkillSeedOutcome> expert_outcomes;
};

const std::vector<DefaultSkillSeed>& default_skill_seeds();
const std::vector<DefaultExpertSeed>& default_expert_seeds();

std::optional<std::filesystem::path> find_default_skill_seed_dir(
    const std::string& argv0_dir = "");

std::filesystem::path default_skill_seed_state_path(
    const std::filesystem::path& acecode_home);

std::filesystem::path default_skill_seed_version_path(
    const std::filesystem::path& acecode_home);

std::filesystem::path packaged_default_skill_seed_version_path(
    const std::filesystem::path& seed_skills_dir);

DefaultSkillSeedInstallResult reconcile_default_global_skills(
    const std::filesystem::path& acecode_home,
    const std::filesystem::path& seed_skills_dir);

DefaultSkillSeedInstallResult reconcile_default_global_skills_on_startup(
    const std::filesystem::path& acecode_home,
    const std::string& argv0_dir);

} // namespace acecode
