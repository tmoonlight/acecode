#include "skills/default_skill_seeder.hpp"
#include "skills/skill_registry.hpp"
#include "utils/sha256.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr const char* kSeedVersion1 = "2026-07-20.1";
constexpr const char* kSeedVersion2 = "2026-07-21.1";
constexpr const char* kSeedVersionNewer = "2027-01-01.1";

fs::path make_temp_root(const std::string& name) {
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() /
        ("acecode-default-skill-seeder-" + name + "-" +
         std::to_string(now));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open()) << path;
    ofs << content;
    ASSERT_TRUE(static_cast<bool>(ofs)) << path;
}

void write_skill_file(const fs::path& dir,
                      const std::string& name,
                      const std::string& description) {
    write_file(
        dir / "SKILL.md",
        "---\n"
        "name: " + name + "\n"
        "description: " + description + "\n"
        "---\n\n"
        "# " + name + "\n\n" +
        description + "\n");
}

void write_seed_version(const fs::path& seed_root,
                        const std::string& version) {
    write_file(seed_root.parent_path() / "seed.version", version + "\n");
}

void write_seed_bundle(const fs::path& seed_root,
                       const std::string& version = kSeedVersion1,
                       const std::string& description_prefix = "seeded ") {
    for (const auto& seed : acecode::default_skill_seeds()) {
        write_skill_file(
            seed_root / seed.relative_path,
            seed.name,
            description_prefix + seed.name);
    }
    write_seed_version(seed_root, version);
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(ifs),
        std::istreambuf_iterator<char>());
}

std::string canonical_lf_sha256(const fs::path& path) {
    const std::string content = read_file(path);
    std::string canonical;
    canonical.reserve(content.size());
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\r' &&
            i + 1 < content.size() &&
            content[i + 1] == '\n') {
            continue;
        }
        canonical.push_back(content[i]);
    }
    return acecode::sha256_hex(canonical);
}

nlohmann::json read_json(const fs::path& path) {
    return nlohmann::json::parse(read_file(path));
}

std::string trim_ascii(std::string value) {
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' ||
            value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

std::string fnv1a64_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::uint64_t hash = 14695981039346656037ULL;
    char buffer[4096];
    while (ifs.good()) {
        ifs.read(buffer, sizeof(buffer));
        const std::streamsize count = ifs.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream oss;
    oss << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
        << hash;
    return oss.str();
}

std::size_t count_outcome(
    const acecode::DefaultSkillSeedInstallResult& result,
    const std::string& value) {
    std::size_t count = 0;
    for (const auto& outcome : result.outcomes) {
        if (outcome.result == value) ++count;
    }
    return count;
}

const acecode::DefaultSkillSeedOutcome* find_outcome(
    const acecode::DefaultSkillSeedInstallResult& result,
    const std::string& name) {
    for (const auto& outcome : result.outcomes) {
        if (outcome.name == name) return &outcome;
    }
    return nullptr;
}

class DefaultSkillSeederTest : public ::testing::Test {
protected:
    fs::path root;
    fs::path home;
    fs::path seed_root;

    void SetUp() override {
        root = make_temp_root("case");
        home = root / "home" / ".acecode";
        seed_root = root / "seed" / "skills";
        write_seed_bundle(seed_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

TEST_F(DefaultSkillSeederTest, ExistingHomeWithoutMarkerReceivesAllDefaults) {
    fs::create_directories(home);

    auto result =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(result.attempted);
    ASSERT_TRUE(result.state_written);
    ASSERT_TRUE(result.version_written);
    EXPECT_EQ(result.bundle_version, kSeedVersion1);
    EXPECT_EQ(
        count_outcome(result, "installed"),
        acecode::default_skill_seeds().size());
    EXPECT_EQ(
        trim_ascii(read_file(
            acecode::default_skill_seed_version_path(home))),
        kSeedVersion1);

    for (const auto& seed : acecode::default_skill_seeds()) {
        EXPECT_TRUE(fs::is_regular_file(
            home / "skills" / seed.relative_path / "SKILL.md"));
    }

    const auto state =
        read_json(acecode::default_skill_seed_state_path(home));
    EXPECT_EQ(state["schema_version"], 2);
    EXPECT_EQ(state["bundle_version"], kSeedVersion1);
    EXPECT_TRUE(state["completed"].get<bool>());
    ASSERT_EQ(
        state["skills"].size(),
        acecode::default_skill_seeds().size());
    for (const auto& item : state["skills"]) {
        EXPECT_TRUE(item["acecode_owned"].get<bool>());
        EXPECT_EQ(item["source_tree_sha256"].get<std::string>().size(), 64u);
        EXPECT_EQ(
            item["installed_tree_sha256"].get<std::string>().size(),
            64u);
    }
}

TEST_F(DefaultSkillSeederTest, SeededSkillsAreVisibleInSameRegistryScan) {
    auto result =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(result.version_written);

    acecode::SkillRegistry registry;
    registry.set_scan_roots({home / "skills"});
    registry.scan();

    EXPECT_EQ(
        registry.list().size(),
        acecode::default_skill_seeds().size());
    for (const auto& seed : acecode::default_skill_seeds()) {
        auto found = registry.find(seed.name);
        ASSERT_TRUE(found.has_value()) << seed.name;
        EXPECT_EQ(found->description, "seeded " + seed.name);
    }
}

TEST_F(DefaultSkillSeederTest, EqualVersionIsANoOp) {
    auto first =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(first.version_written);

    const auto& seed = acecode::default_skill_seeds().front();
    const fs::path target = home / "skills" / seed.relative_path;
    write_skill_file(target, seed.name, "changed after reconciliation");

    auto second =
        acecode::reconcile_default_global_skills(home, seed_root);

    EXPECT_FALSE(second.attempted);
    EXPECT_FALSE(second.version_written);
    EXPECT_TRUE(second.outcomes.empty());
    EXPECT_NE(
        read_file(target / "SKILL.md").find(
            "changed after reconciliation"),
        std::string::npos);
}

TEST_F(DefaultSkillSeederTest, PreservesUnknownExistingTarget) {
    const auto& seed = acecode::default_skill_seeds().front();
    const fs::path existing = home / "skills" / seed.relative_path;
    write_skill_file(existing, seed.name, "user copy");

    auto result =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(result.version_written);
    EXPECT_EQ(count_outcome(result, "preserved_user_modified"), 1u);
    EXPECT_EQ(
        count_outcome(result, "installed"),
        acecode::default_skill_seeds().size() - 1);
    EXPECT_NE(
        read_file(existing / "SKILL.md").find("user copy"),
        std::string::npos);

    const auto* outcome = find_outcome(result, seed.name);
    ASSERT_NE(outcome, nullptr);
    EXPECT_FALSE(outcome->acecode_owned);
}

TEST_F(DefaultSkillSeederTest, UpdatesPristineAcecodeOwnedSeed) {
    auto first =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(first.version_written);

    const auto& seed = acecode::default_skill_seeds().front();
    write_skill_file(
        seed_root / seed.relative_path,
        seed.name,
        "new bundled copy");
    write_seed_version(seed_root, kSeedVersion2);

    auto second =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(second.version_written);
    EXPECT_EQ(count_outcome(second, "updated"), 1u);
    EXPECT_EQ(
        count_outcome(second, "unchanged"),
        acecode::default_skill_seeds().size() - 1);
    EXPECT_NE(
        read_file(
            home / "skills" / seed.relative_path / "SKILL.md")
            .find("new bundled copy"),
        std::string::npos);

    const auto* outcome = find_outcome(second, seed.name);
    ASSERT_NE(outcome, nullptr);
    EXPECT_TRUE(outcome->acecode_owned);
    EXPECT_EQ(
        outcome->source_tree_sha256,
        outcome->installed_tree_sha256);
}

TEST_F(DefaultSkillSeederTest, PreservesModifiedAcecodeOwnedSeed) {
    auto first =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(first.version_written);

    const auto& seed = acecode::default_skill_seeds().front();
    const fs::path target = home / "skills" / seed.relative_path;
    write_skill_file(target, seed.name, "user-modified seeded copy");
    write_skill_file(
        seed_root / seed.relative_path,
        seed.name,
        "new bundled copy");
    write_seed_version(seed_root, kSeedVersion2);

    auto second =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(second.version_written);
    const auto* outcome = find_outcome(second, seed.name);
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "preserved_user_modified");
    EXPECT_FALSE(outcome->acecode_owned);
    EXPECT_NE(
        read_file(target / "SKILL.md").find(
            "user-modified seeded copy"),
        std::string::npos);
    EXPECT_EQ(
        trim_ascii(read_file(
            acecode::default_skill_seed_version_path(home))),
        kSeedVersion2);
}

TEST_F(DefaultSkillSeederTest, FullTreeHashDetectsModifiedSupportingFile) {
    const auto& seed = acecode::default_skill_seeds().front();
    write_file(
        seed_root / seed.relative_path / "agents" / "openai.yaml",
        "version: 1\n");

    auto first =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(first.version_written);

    const fs::path target = home / "skills" / seed.relative_path;
    write_file(
        target / "agents" / "openai.yaml",
        "version: user\n");
    write_skill_file(
        seed_root / seed.relative_path,
        seed.name,
        "new bundled copy");
    write_file(
        seed_root / seed.relative_path / "agents" / "openai.yaml",
        "version: 2\n");
    write_seed_version(seed_root, kSeedVersion2);

    auto second =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(second.version_written);
    const auto* outcome = find_outcome(second, seed.name);
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "preserved_user_modified");
    EXPECT_EQ(
        read_file(target / "agents" / "openai.yaml"),
        "version: user\n");
}

TEST_F(DefaultSkillSeederTest, FullTreeHashDetectsAddedEmptyDirectory) {
    auto first =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(first.version_written);

    const auto& seed = acecode::default_skill_seeds().front();
    const fs::path target = home / "skills" / seed.relative_path;
    const fs::path user_directory = target / "user-empty-directory";
    ASSERT_TRUE(fs::create_directories(user_directory));
    write_skill_file(
        seed_root / seed.relative_path,
        seed.name,
        "new bundled copy");
    write_seed_version(seed_root, kSeedVersion2);

    auto second =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(second.version_written);
    const auto* outcome = find_outcome(second, seed.name);
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "preserved_user_modified");
    EXPECT_TRUE(fs::is_directory(user_directory));
}

TEST_F(DefaultSkillSeederTest, NewerUserMarkerPreventsDowngrade) {
    write_file(
        acecode::default_skill_seed_version_path(home),
        std::string(kSeedVersionNewer) + "\n");

    auto result =
        acecode::reconcile_default_global_skills(home, seed_root);

    EXPECT_FALSE(result.attempted);
    EXPECT_TRUE(result.downgrade_skipped);
    EXPECT_FALSE(result.version_written);
    EXPECT_TRUE(result.outcomes.empty());
    EXPECT_FALSE(fs::exists(home / "skills"));
    EXPECT_EQ(
        trim_ascii(read_file(
            acecode::default_skill_seed_version_path(home))),
        kSeedVersionNewer);
}

TEST_F(DefaultSkillSeederTest, InvalidPackagedVersionIsNonDestructive) {
    write_file(seed_root.parent_path() / "seed.version", "tomorrow\n");

    auto result =
        acecode::reconcile_default_global_skills(home, seed_root);

    EXPECT_TRUE(result.attempted);
    EXPECT_FALSE(result.error.empty());
    EXPECT_FALSE(result.state_written);
    EXPECT_FALSE(result.version_written);
    EXPECT_FALSE(fs::exists(home / "skills"));
    EXPECT_FALSE(fs::exists(
        acecode::default_skill_seed_version_path(home)));
}

TEST_F(DefaultSkillSeederTest, PartialFailureDoesNotAdvanceAndCanRetry) {
    const auto& failed_seed = acecode::default_skill_seeds().front();
    fs::remove(seed_root / failed_seed.relative_path / "SKILL.md");

    auto first =
        acecode::reconcile_default_global_skills(home, seed_root);

    EXPECT_TRUE(first.attempted);
    EXPECT_FALSE(first.error.empty());
    EXPECT_TRUE(first.state_written);
    EXPECT_FALSE(first.version_written);
    EXPECT_FALSE(fs::exists(
        acecode::default_skill_seed_version_path(home)));
    EXPECT_EQ(count_outcome(first, "missing_source"), 1u);

    write_skill_file(
        seed_root / failed_seed.relative_path,
        failed_seed.name,
        "restored source");

    auto second =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(second.version_written);
    EXPECT_TRUE(second.error.empty());
    EXPECT_EQ(count_outcome(second, "installed"), 1u);
    EXPECT_EQ(
        count_outcome(second, "unchanged"),
        acecode::default_skill_seeds().size() - 1);
}

TEST_F(DefaultSkillSeederTest, RetryPreservesPriorOwnershipAfterSourceFailure) {
    auto initial =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(initial.version_written);

    const auto& failed_seed = acecode::default_skill_seeds().front();
    fs::remove(seed_root / failed_seed.relative_path / "SKILL.md");
    write_seed_version(seed_root, kSeedVersion2);

    auto failed =
        acecode::reconcile_default_global_skills(home, seed_root);

    EXPECT_TRUE(failed.state_written);
    EXPECT_FALSE(failed.version_written);
    EXPECT_EQ(count_outcome(failed, "missing_source"), 1u);

    write_skill_file(
        seed_root / failed_seed.relative_path,
        failed_seed.name,
        "repaired newer source");

    auto retried =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(retried.version_written);
    const auto* outcome = find_outcome(retried, failed_seed.name);
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "updated");
    EXPECT_TRUE(outcome->acecode_owned);
    EXPECT_NE(
        read_file(
            home / "skills" / failed_seed.relative_path / "SKILL.md")
            .find("repaired newer source"),
        std::string::npos);
}

TEST_F(DefaultSkillSeederTest, MigratesPristineLegacySkillMdOnlyState) {
    const auto& legacy_seed = acecode::default_skill_seeds().front();
    const fs::path target =
        home / "skills" / legacy_seed.relative_path;
    write_skill_file(target, legacy_seed.name, "legacy installed copy");

    nlohmann::json state;
    state["bundle_version"] = "2026-07-19.1";
    state["skills"] = nlohmann::json::array({
        {
            {"name", legacy_seed.name},
            {"source_id", legacy_seed.source_id},
            {"relative_path", legacy_seed.relative_path.generic_string()},
            {"result", "installed"},
            {"skill_md_hash", fnv1a64_file(target / "SKILL.md")},
        },
    });
    write_file(
        acecode::default_skill_seed_state_path(home),
        state.dump(2) + "\n");
    write_file(
        acecode::default_skill_seed_version_path(home),
        "2026-07-19.1\n");

    auto result =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(result.version_written);
    const auto* outcome = find_outcome(result, legacy_seed.name);
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "updated");
    EXPECT_TRUE(outcome->acecode_owned);
    EXPECT_NE(
        read_file(target / "SKILL.md").find(
            "seeded " + legacy_seed.name),
        std::string::npos);
}

TEST_F(DefaultSkillSeederTest, ConcurrentCallsProduceOneConsistentState) {
    auto future1 = std::async(std::launch::async, [&] {
        return acecode::reconcile_default_global_skills(home, seed_root);
    });
    auto future2 = std::async(std::launch::async, [&] {
        return acecode::reconcile_default_global_skills(home, seed_root);
    });

    auto result1 = future1.get();
    auto result2 = future2.get();

    EXPECT_EQ(
        static_cast<int>(result1.attempted) +
            static_cast<int>(result2.attempted),
        1);
    EXPECT_EQ(
        static_cast<int>(result1.version_written) +
            static_cast<int>(result2.version_written),
        1);
    EXPECT_TRUE(result1.error.empty());
    EXPECT_TRUE(result2.error.empty());

    const auto state =
        read_json(acecode::default_skill_seed_state_path(home));
    EXPECT_TRUE(state["completed"].get<bool>());
    EXPECT_EQ(
        state["skills"].size(),
        acecode::default_skill_seeds().size());
    EXPECT_EQ(
        trim_ascii(read_file(
            acecode::default_skill_seed_version_path(home))),
        kSeedVersion1);
}

TEST_F(DefaultSkillSeederTest, AgentRootDiscoveryAndPrecedenceSurviveSeeding) {
    auto result =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(result.version_written);

    const fs::path project_agent_root =
        root / "project" / ".agent" / "skills";
    write_skill_file(
        project_agent_root / "skill-management" / "find-skills",
        "find-skills",
        "project agent override");
    write_skill_file(
        project_agent_root / "ops" / "agent-only",
        "agent-only",
        "agent compatible skill");

    acecode::SkillRegistry registry;
    registry.set_scan_roots({project_agent_root, home / "skills"});
    registry.scan();

    auto overridden = registry.find("find-skills");
    ASSERT_TRUE(overridden.has_value());
    EXPECT_EQ(overridden->description, "project agent override");

    auto agent_only = registry.find("agent-only");
    ASSERT_TRUE(agent_only.has_value());
    EXPECT_EQ(agent_only->description, "agent compatible skill");
}

TEST(DefaultSkillSeedRegistryTest, PackagedManifestVersionAndHashesAgree) {
    const fs::path source_file = fs::absolute(fs::path(__FILE__));
    const fs::path repository_root =
        source_file.parent_path().parent_path().parent_path();
    const fs::path seed_root = repository_root / "assets" / "seed";

    const std::string version =
        trim_ascii(read_file(seed_root / "seed.version"));
    const auto manifest = read_json(seed_root / "MANIFEST.json");
    ASSERT_TRUE(manifest.contains("bundle_version"));
    EXPECT_EQ(manifest["bundle_version"].get<std::string>(), version);

    ASSERT_TRUE(manifest["skills"].is_array());
    EXPECT_EQ(
        manifest["skills"].size(),
        acecode::default_skill_seeds().size());

    std::set<std::string> manifest_names;
    for (const auto& item : manifest["skills"]) {
        const std::string name = item["name"].get<std::string>();
        const std::string relative_path =
            item["relative_path"].get<std::string>();
        const fs::path skill_md =
            seed_root / "skills" / relative_path / "SKILL.md";
        manifest_names.insert(name);
        ASSERT_TRUE(fs::is_regular_file(skill_md)) << skill_md;
        EXPECT_EQ(
            canonical_lf_sha256(skill_md),
            item["skill_md_sha256"].get<std::string>())
            << name;
    }

    for (const auto& seed : acecode::default_skill_seeds()) {
        EXPECT_EQ(manifest_names.count(seed.name), 1u) << seed.name;
    }
}

TEST(DefaultSkillSeedRegistryTest, AcecodeTuiUsageSkillIsRegisteredInSeedBundle) {
    bool found = false;
    for (const auto& seed : acecode::default_skill_seeds()) {
        if (seed.name == "acecode-tui-usage") {
            found = true;
            EXPECT_EQ(
                seed.relative_path.generic_string(),
                "acecode/acecode-tui-usage");
            EXPECT_NE(
                seed.source_id.find("acecode:acecode-tui-usage"),
                std::string::npos);
            break;
        }
    }
    EXPECT_TRUE(found)
        << "acecode-tui-usage seed must stay registered";
}

TEST(DefaultSkillSeedRegistryTest, AcecodeDesktopUsageSkillIsRegisteredInSeedBundle) {
    bool found = false;
    for (const auto& seed : acecode::default_skill_seeds()) {
        if (seed.name == "acecode-desktop-usage") {
            found = true;
            EXPECT_EQ(
                seed.relative_path.generic_string(),
                "acecode/acecode-desktop-usage");
            EXPECT_NE(
                seed.source_id.find("acecode:acecode-desktop-usage"),
                std::string::npos);
            break;
        }
    }
    EXPECT_TRUE(found)
        << "acecode-desktop-usage seed must stay registered";
}

TEST(DefaultSkillSeedRegistryTest, VisionImageReaderSkillIsRegisteredInSeedBundle) {
    bool found = false;
    for (const auto& seed : acecode::default_skill_seeds()) {
        if (seed.name == "vision-image-reader") {
            found = true;
            EXPECT_EQ(
                seed.relative_path.generic_string(),
                "acecode/vision-image-reader");
            EXPECT_NE(
                seed.source_id.find("acecode:vision-image-reader"),
                std::string::npos);
            break;
        }
    }
    EXPECT_TRUE(found)
        << "vision-image-reader seed must stay registered";
}

} // namespace
