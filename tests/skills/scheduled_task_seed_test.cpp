#include <gtest/gtest.h>

#include "skills/default_skill_seeder.hpp"
#include "skills/skill_registry.hpp"
#include "test_support/repo_root.hpp"
#include "utils/uuid.hpp"

#include <filesystem>
#include <fstream>

TEST(ScheduledTaskSeed, UpgradeInstallsDiscoverableSkillWithoutChangingUserOverride) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("acecode-scheduled-seed-" + acecode::generate_uuid());
    const auto packaged = acecode::test_support::find_repo_root(__FILE__) / "assets" / "seed";
    fs::create_directories(root);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{root};
    std::ofstream(root / "seed.version") << "2026-09-28.1\n";
    const auto installed = acecode::reconcile_default_global_skills(root, packaged / "skills");
    ASSERT_TRUE(installed.error.empty()) << installed.error;
    EXPECT_TRUE(installed.version_written);
    acecode::SkillRegistry registry;
    registry.set_scan_roots({root / "skills"});
    registry.scan();
    const auto meta = registry.find("scheduled-task");
    ASSERT_TRUE(meta);
    EXPECT_EQ(meta->command_key, "scheduled-task");
    EXPECT_FALSE(registry.read_skill_body("scheduled-task").empty());

    const auto skill_path = root / "skills" / "acecode" / "scheduled-task" / "SKILL.md";
    const std::string custom = "---\nname: scheduled-task\ndescription: Custom scheduling\n---\nMy instructions\n";
    std::ofstream(skill_path, std::ios::binary) << custom;
    const auto repeated = acecode::reconcile_default_global_skills(root, packaged / "skills");
    EXPECT_TRUE(repeated.error.empty()) << repeated.error;
    registry.scan();
    EXPECT_EQ(registry.read_skill_text("scheduled-task"), custom);
}
