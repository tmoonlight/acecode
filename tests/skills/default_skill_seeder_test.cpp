#include "skills/default_skill_seeder.hpp"
#include "skills/skill_registry.hpp"
#include "experts/expert_registry.hpp"
#include "hooks/hook_registry.hpp"
#include "hooks/hook_runner.hpp"
#include "utils/sha256.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kSeedVersion1 = "2026-07-20.1";
constexpr const char* kSeedVersion2 = "2026-07-21.1";
constexpr const char* kSeedVersionNewer = "2027-01-01.1";

void set_environment_value(
    const char* name,
    const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(name, value ? value->c_str() : "");
#else
    if (value) {
        setenv(name, value->c_str(), 1);
    } else {
        unsetenv(name);
    }
#endif
}

class ScopedEnvironmentValue {
public:
    ScopedEnvironmentValue(
        const char* name,
        const std::optional<std::string>& value)
        : name_(name) {
        if (const char* current = std::getenv(name)) {
            previous_ = std::string(current);
        }
        set_environment_value(name_.c_str(), value);
    }

    ~ScopedEnvironmentValue() {
        set_environment_value(name_.c_str(), previous_);
    }

    ScopedEnvironmentValue(const ScopedEnvironmentValue&) = delete;
    ScopedEnvironmentValue& operator=(const ScopedEnvironmentValue&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

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

void write_expert_package(const fs::path& dir,
                          const acecode::DefaultExpertSeed& seed) {
    nlohmann::json manifest = {
        {"name", seed.name},
        {"version", "1.0.0"},
        {"expertType", "agent"},
        {"displayName", seed.name},
        {"displayDescription", "seeded " + seed.name},
        {"agentName", "lead"},
        {"agents",
         nlohmann::json::array({
             {
                 {"id", "lead"},
                 {"path", "agents/lead.md"},
                 {"displayName", seed.name},
             },
         })},
    };
    if (seed.name == "opc-team") {
        manifest.erase("agentName");
        manifest.erase("agents");
        manifest["expertType"] = "team";
        manifest["teamInfo"] = {
            {"leadExpert", "opc-team-lead"},
            {"memberExperts",
             nlohmann::json::array({
                 "opc-resource-auditor",
                 "opc-niche-strategist",
                 "opc-value-designer",
                 "opc-model-architect",
                 "opc-mvp-designer",
                 "opc-conversion-designer",
                 "opc-asset-strategist",
                 "opc-dashboard-reviewer",
             })},
        };
    } else {
        write_file(
            dir / "agents" / "lead.md",
            "---\n"
            "id: lead\n"
            "displayName: " + seed.name + "\n"
            "---\n\n"
            "You are " + seed.name + ".\n");
    }
    write_file(dir / "expert.json", manifest.dump(2) + "\n");
}

void write_hook_package(const fs::path& dir,
                        const std::string& command = "seeded-hook") {
    nlohmann::json config = {
        {"hooks",
         {
             {"SessionStart",
              nlohmann::json::array({
                  {
                      {"matcher", "*"},
                      {"hooks",
                       nlohmann::json::array({
                           {
                               {"type", "command"},
                               {"command", command},
                           },
                       })},
                  },
              })},
         }},
    };
    write_file(dir / "hooks.json", config.dump(2) + "\n");
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
    for (const auto& seed : acecode::default_expert_seeds()) {
        write_expert_package(
            seed_root.parent_path() / "experts" / seed.relative_path,
            seed);
    }
    for (const auto& seed : acecode::default_hook_seeds()) {
        write_hook_package(
            seed_root.parent_path() / "hooks" / seed.relative_path);
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

std::size_t count_expert_outcome(
    const acecode::DefaultSkillSeedInstallResult& result,
    const std::string& value) {
    std::size_t count = 0;
    for (const auto& outcome : result.expert_outcomes) {
        if (outcome.result == value) ++count;
    }
    return count;
}

std::size_t count_hook_outcome(
    const acecode::DefaultSkillSeedInstallResult& result,
    const std::string& value) {
    std::size_t count = 0;
    for (const auto& outcome : result.hook_outcomes) {
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

const acecode::DefaultSkillSeedOutcome* find_expert_outcome(
    const acecode::DefaultSkillSeedInstallResult& result,
    const std::string& name) {
    for (const auto& outcome : result.expert_outcomes) {
        if (outcome.name == name) return &outcome;
    }
    return nullptr;
}

const acecode::DefaultSkillSeedOutcome* find_hook_outcome(
    const acecode::DefaultSkillSeedInstallResult& result,
    const std::string& name) {
    for (const auto& outcome : result.hook_outcomes) {
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
        count_expert_outcome(result, "installed"),
        acecode::default_expert_seeds().size());
    EXPECT_EQ(
        count_hook_outcome(result, "installed"),
        acecode::default_hook_seeds().size());
    EXPECT_EQ(
        trim_ascii(read_file(
            acecode::default_skill_seed_version_path(home))),
        kSeedVersion1);

    for (const auto& seed : acecode::default_skill_seeds()) {
        EXPECT_TRUE(fs::is_regular_file(
            home / "skills" / seed.relative_path / "SKILL.md"));
    }
    for (const auto& seed : acecode::default_expert_seeds()) {
        EXPECT_TRUE(fs::is_regular_file(
            home / "experts" / seed.relative_path / "expert.json"));
    }
    for (const auto& seed : acecode::default_hook_seeds()) {
        EXPECT_TRUE(fs::is_regular_file(
            home / "hooks" / seed.relative_path / "hooks.json"));
    }

    const auto state =
        read_json(acecode::default_skill_seed_state_path(home));
    EXPECT_EQ(state["schema_version"], 4);
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
    ASSERT_EQ(
        state["experts"].size(),
        acecode::default_expert_seeds().size());
    for (const auto& item : state["experts"]) {
        EXPECT_TRUE(item["acecode_owned"].get<bool>());
        EXPECT_EQ(item["source_tree_sha256"].get<std::string>().size(), 64u);
        EXPECT_EQ(
            item["installed_tree_sha256"].get<std::string>().size(),
            64u);
    }
    ASSERT_EQ(
        state["hooks"].size(),
        acecode::default_hook_seeds().size());
    for (const auto& item : state["hooks"]) {
        EXPECT_TRUE(item["acecode_owned"].get<bool>());
        EXPECT_EQ(item["source_tree_sha256"].get<std::string>().size(), 64u);
        EXPECT_EQ(
            item["installed_tree_sha256"].get<std::string>().size(),
            64u);
    }
}

TEST_F(DefaultSkillSeederTest, HookSeedDoesNotRewriteUserHookFiles) {
    const fs::path user_hooks = home / "hooks.json";
    const std::string user_config =
        "{\"hooks\":{\"Stop\":[{\"hooks\":[{\"type\":\"command\","
        "\"command\":\"user-hook\"}]}]}}\n";
    write_file(user_hooks, user_config);

    const auto result =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(result.version_written);
    EXPECT_EQ(read_file(user_hooks), user_config);
    ASSERT_EQ(result.hook_outcomes.size(), 1u);
    EXPECT_EQ(result.hook_outcomes[0].result, "installed");
    EXPECT_TRUE(fs::is_regular_file(
        home / "hooks" / "agent-reporting" / "hooks.json"));
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

TEST_F(DefaultSkillSeederTest, SeededExpertsAreVisibleInSameRegistryScan) {
    auto result =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(result.version_written);

    acecode::ExpertRegistry registry(home / "experts");
    std::vector<acecode::ExpertDiagnostic> diagnostics;
    const auto experts = registry.list(root.string(), &diagnostics);

    EXPECT_TRUE(diagnostics.empty());
    EXPECT_EQ(experts.size(), acecode::default_expert_seeds().size());
    for (const auto& seed : acecode::default_expert_seeds()) {
        auto found = registry.find(root.string(), seed.name);
        ASSERT_TRUE(found.has_value()) << seed.name;
        EXPECT_EQ(found->display_name, seed.name);
    }
}

TEST_F(DefaultSkillSeederTest, DiscoversPortableUpdaterLayout) {
    const fs::path executable_dir = root / "portable";
    const fs::path portable_seed =
        executable_dir / "share" / "acecode" / "seed" / "skills";
    write_seed_bundle(portable_seed);

    const auto found =
        acecode::find_default_skill_seed_dir(executable_dir.string());

    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(fs::equivalent(*found, portable_seed));
}

TEST_F(DefaultSkillSeederTest, DiscoversInstalledBinShareLayout) {
    const fs::path install_root = root / "installed";
    const fs::path executable_dir = install_root / "bin";
    const fs::path installed_seed =
        install_root / "share" / "acecode" / "seed" / "skills";
    fs::create_directories(executable_dir);
    write_seed_bundle(installed_seed);

    const auto found =
        acecode::find_default_skill_seed_dir(executable_dir.string());

    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(fs::equivalent(*found, installed_seed));
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
    EXPECT_TRUE(second.expert_outcomes.empty());
    EXPECT_TRUE(second.hook_outcomes.empty());
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

TEST_F(DefaultSkillSeederTest, PreservesUserCreatedExpertWithSeededId) {
    const auto& seed = acecode::default_expert_seeds().front();
    const fs::path existing =
        home / "experts" / seed.relative_path;
    write_expert_package(existing, seed);
    auto manifest = read_json(existing / "expert.json");
    manifest["displayName"] = "user copy";
    write_file(existing / "expert.json", manifest.dump(2) + "\n");

    auto result =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(result.version_written);
    EXPECT_EQ(
        count_expert_outcome(result, "preserved_user_modified"),
        1u);
    EXPECT_EQ(
        count_expert_outcome(result, "installed"),
        acecode::default_expert_seeds().size() - 1);
    EXPECT_EQ(
        read_json(existing / "expert.json")["displayName"],
        "user copy");

    const auto* outcome = find_expert_outcome(result, seed.name);
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

TEST_F(DefaultSkillSeederTest, UpdatesPristineAcecodeOwnedExpert) {
    auto first =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(first.version_written);

    const auto& seed = acecode::default_expert_seeds().front();
    const fs::path bundled =
        seed_root.parent_path() / "experts" / seed.relative_path;
    auto manifest = read_json(bundled / "expert.json");
    manifest["displayName"] = "new bundled expert";
    write_file(bundled / "expert.json", manifest.dump(2) + "\n");
    write_seed_version(seed_root, kSeedVersion2);

    auto second =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(second.version_written);
    EXPECT_EQ(count_expert_outcome(second, "updated"), 1u);
    EXPECT_EQ(
        count_expert_outcome(second, "unchanged"),
        acecode::default_expert_seeds().size() - 1);
    EXPECT_EQ(
        read_json(
            home / "experts" / seed.relative_path / "expert.json")
            ["displayName"],
        "new bundled expert");
}

TEST_F(DefaultSkillSeederTest, UpdatesPristineAcecodeOwnedHook) {
    auto first =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(first.version_written);

    const auto& seed = acecode::default_hook_seeds().front();
    write_hook_package(
        seed_root.parent_path() / "hooks" / seed.relative_path,
        "new-bundled-hook");
    write_seed_version(seed_root, kSeedVersion2);

    auto second =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(second.version_written);
    EXPECT_EQ(count_hook_outcome(second, "updated"), 1u);
    EXPECT_NE(
        read_file(
            home / "hooks" / seed.relative_path / "hooks.json")
            .find("new-bundled-hook"),
        std::string::npos);
}

TEST_F(DefaultSkillSeederTest, PreservesModifiedAcecodeOwnedHook) {
    auto first =
        acecode::reconcile_default_global_skills(home, seed_root);
    ASSERT_TRUE(first.version_written);

    const auto& seed = acecode::default_hook_seeds().front();
    const fs::path target = home / "hooks" / seed.relative_path;
    write_hook_package(target, "user-modified-hook");
    write_hook_package(
        seed_root.parent_path() / "hooks" / seed.relative_path,
        "new-bundled-hook");
    write_seed_version(seed_root, kSeedVersion2);

    auto second =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(second.version_written);
    const auto* outcome = find_hook_outcome(second, seed.name);
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "preserved_user_modified");
    EXPECT_FALSE(outcome->acecode_owned);
    EXPECT_NE(
        read_file(target / "hooks.json").find("user-modified-hook"),
        std::string::npos);
}

TEST(DefaultSkillSeedRegistryTest,
     PackagedHookRepairsDriftedStateForKnownPreviousOfficialDefinition) {
    const fs::path source_file = fs::absolute(fs::path(__FILE__));
    const fs::path repository_root =
        source_file.parent_path().parent_path().parent_path();
    const fs::path packaged_seed = repository_root / "assets" / "seed";
    const fs::path temp_root = make_temp_root("known-official-hook-upgrade");
    const fs::path home = temp_root / "profile" / ".acecode";
    const fs::path target = home / "hooks" / "agent-reporting";

    auto previous_official = read_json(
        packaged_seed / "hooks" / "agent-reporting" / "hooks.json");
    ASSERT_EQ(previous_official["hooks"].erase("SessionTitleChanged"), 1u);
    const auto& hook_seed = acecode::default_hook_seeds().front();
    ASSERT_NE(
        std::find(
            hook_seed.previous_definition_sha256s.begin(),
            hook_seed.previous_definition_sha256s.end(),
            acecode::sha256_hex(previous_official.dump())),
        hook_seed.previous_definition_sha256s.end())
        << "fixture must remain an exact known previous official definition";
    write_file(target / "hooks.json", previous_official.dump(2) + "\n");
    write_file(home / "seed.version", "2026-08-12.2\n");
    write_file(
        home / ".seed_skills_state.json",
        nlohmann::json{
            {"schema_version", 4},
            {"bundle_version", "2026-08-12.2"},
            {"completed", true},
            {"skills", nlohmann::json::array()},
            {"experts", nlohmann::json::array()},
            {"hooks",
             nlohmann::json::array({
                 {
                     {"name", "agent-reporting"},
                     {"source_id", "acecode:managed-hook/agent-reporting@2026-08-12"},
                     {"relative_path", "agent-reporting"},
                     {"result", "preserved_user_modified"},
                     {"acecode_owned", false},
                     {"installed_tree_sha256", "drifted-state-hash"},
                 },
             })},
        }.dump(2) + "\n");

    const auto result = acecode::reconcile_default_global_skills(
        home, packaged_seed / "skills");

    ASSERT_TRUE(result.version_written) << result.error;
    const auto* outcome = find_hook_outcome(result, "agent-reporting");
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "updated");
    EXPECT_TRUE(outcome->acecode_owned);
    EXPECT_TRUE(read_json(target / "hooks.json")["hooks"].contains(
        "SessionTitleChanged"));
    EXPECT_EQ(trim_ascii(read_file(home / "seed.version")), "2026-08-14.1");
    const auto state = read_json(home / ".seed_skills_state.json");
    ASSERT_EQ(state["hooks"].size(), 1u);
    EXPECT_TRUE(state["hooks"][0]["acecode_owned"].get<bool>());

    std::error_code cleanup_error;
    fs::remove_all(temp_root, cleanup_error);
}

TEST(DefaultSkillSeedRegistryTest,
     PackagedHookKeepsActuallyModifiedDefinitionWhenStateHasDrifted) {
    const fs::path source_file = fs::absolute(fs::path(__FILE__));
    const fs::path repository_root =
        source_file.parent_path().parent_path().parent_path();
    const fs::path packaged_seed = repository_root / "assets" / "seed";
    const fs::path temp_root = make_temp_root("modified-hook-drift");
    const fs::path home = temp_root / "profile" / ".acecode";
    const fs::path target = home / "hooks" / "agent-reporting";

    auto modified = read_json(
        packaged_seed / "hooks" / "agent-reporting" / "hooks.json");
    ASSERT_EQ(modified["hooks"].erase("SessionTitleChanged"), 1u);
    modified["hooks"]["SessionStart"][0]["hooks"][0]["command"] =
        "user-owned-command";
    write_file(target / "hooks.json", modified.dump(2) + "\n");
    write_file(home / "seed.version", "2026-08-12.2\n");
    write_file(
        home / ".seed_skills_state.json",
        nlohmann::json{
            {"schema_version", 4},
            {"bundle_version", "2026-08-12.2"},
            {"completed", true},
            {"skills", nlohmann::json::array()},
            {"experts", nlohmann::json::array()},
            {"hooks",
             nlohmann::json::array({
                 {
                     {"name", "agent-reporting"},
                     {"relative_path", "agent-reporting"},
                     {"result", "preserved_user_modified"},
                     {"acecode_owned", false},
                 },
             })},
        }.dump(2) + "\n");

    const auto result = acecode::reconcile_default_global_skills(
        home, packaged_seed / "skills");

    ASSERT_TRUE(result.version_written) << result.error;
    const auto* outcome = find_hook_outcome(result, "agent-reporting");
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "preserved_user_modified");
    EXPECT_FALSE(outcome->acecode_owned);
    const auto installed = read_json(target / "hooks.json");
    EXPECT_EQ(
        installed["hooks"]["SessionStart"][0]["hooks"][0]["command"],
        "user-owned-command");
    EXPECT_FALSE(installed["hooks"].contains("SessionTitleChanged"));

    std::error_code cleanup_error;
    fs::remove_all(temp_root, cleanup_error);
}

TEST(DefaultSkillSeedRegistryTest,
     PackagedHookKeepsKnownOfficialDefinitionWithExtraDirectory) {
    const fs::path source_file = fs::absolute(fs::path(__FILE__));
    const fs::path repository_root =
        source_file.parent_path().parent_path().parent_path();
    const fs::path packaged_seed = repository_root / "assets" / "seed";
    const fs::path temp_root = make_temp_root("official-hook-extra-directory");
    const fs::path home = temp_root / "profile" / ".acecode";
    const fs::path target = home / "hooks" / "agent-reporting";

    auto previous_official = read_json(
        packaged_seed / "hooks" / "agent-reporting" / "hooks.json");
    ASSERT_EQ(previous_official["hooks"].erase("SessionTitleChanged"), 1u);
    write_file(target / "hooks.json", previous_official.dump(2) + "\n");
    ASSERT_TRUE(fs::create_directories(target / "user-data"));
    write_file(home / "seed.version", "2026-08-12.2\n");
    write_file(
        home / ".seed_skills_state.json",
        nlohmann::json{
            {"schema_version", 4},
            {"bundle_version", "2026-08-12.2"},
            {"completed", true},
            {"skills", nlohmann::json::array()},
            {"experts", nlohmann::json::array()},
            {"hooks",
             nlohmann::json::array({
                 {
                     {"name", "agent-reporting"},
                     {"relative_path", "agent-reporting"},
                     {"result", "preserved_user_modified"},
                     {"acecode_owned", false},
                 },
             })},
        }.dump(2) + "\n");

    const auto result = acecode::reconcile_default_global_skills(
        home, packaged_seed / "skills");

    ASSERT_TRUE(result.version_written) << result.error;
    const auto* outcome = find_hook_outcome(result, "agent-reporting");
    ASSERT_NE(outcome, nullptr);
    EXPECT_EQ(outcome->result, "preserved_user_modified");
    EXPECT_FALSE(outcome->acecode_owned);
    EXPECT_FALSE(read_json(target / "hooks.json")["hooks"].contains(
        "SessionTitleChanged"));
    EXPECT_TRUE(fs::is_directory(target / "user-data"));

    std::error_code cleanup_error;
    fs::remove_all(temp_root, cleanup_error);
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

TEST_F(DefaultSkillSeederTest, MissingExpertDoesNotAdvanceAndCanRetry) {
    const auto& failed_seed = acecode::default_expert_seeds().front();
    const fs::path source =
        seed_root.parent_path() / "experts" /
        failed_seed.relative_path / "expert.json";
    fs::remove(source);

    auto first =
        acecode::reconcile_default_global_skills(home, seed_root);

    EXPECT_TRUE(first.attempted);
    EXPECT_FALSE(first.error.empty());
    EXPECT_TRUE(first.state_written);
    EXPECT_FALSE(first.version_written);
    EXPECT_FALSE(fs::exists(
        acecode::default_skill_seed_version_path(home)));
    EXPECT_EQ(count_expert_outcome(first, "missing_source"), 1u);

    write_expert_package(
        seed_root.parent_path() / "experts" /
            failed_seed.relative_path,
        failed_seed);

    auto second =
        acecode::reconcile_default_global_skills(home, seed_root);

    ASSERT_TRUE(second.version_written);
    EXPECT_TRUE(second.error.empty());
    EXPECT_EQ(count_expert_outcome(second, "installed"), 1u);
    EXPECT_EQ(
        count_expert_outcome(second, "unchanged"),
        acecode::default_expert_seeds().size() - 1);
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
        state["experts"].size(),
        acecode::default_expert_seeds().size());
    EXPECT_EQ(
        state["hooks"].size(),
        acecode::default_hook_seeds().size());
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

    ASSERT_TRUE(manifest["experts"].is_array());
    EXPECT_EQ(
        manifest["experts"].size(),
        acecode::default_expert_seeds().size());
    std::set<std::string> manifest_expert_names;
    for (const auto& item : manifest["experts"]) {
        const std::string name = item["name"].get<std::string>();
        const std::string relative_path =
            item["relative_path"].get<std::string>();
        const fs::path expert_json =
            seed_root / "experts" / relative_path / "expert.json";
        manifest_expert_names.insert(name);
        ASSERT_TRUE(fs::is_regular_file(expert_json)) << expert_json;
        EXPECT_EQ(
            canonical_lf_sha256(expert_json),
            item["expert_json_sha256"].get<std::string>())
            << name;
    }
    for (const auto& seed : acecode::default_expert_seeds()) {
        EXPECT_EQ(manifest_expert_names.count(seed.name), 1u)
            << seed.name;
    }

    ASSERT_TRUE(manifest["hooks"].is_array());
    EXPECT_EQ(
        manifest["hooks"].size(),
        acecode::default_hook_seeds().size());
    std::set<std::string> manifest_hook_names;
    for (const auto& item : manifest["hooks"]) {
        const std::string name = item["name"].get<std::string>();
        const std::string relative_path =
            item["relative_path"].get<std::string>();
        const fs::path hooks_json =
            seed_root / "hooks" / relative_path / "hooks.json";
        manifest_hook_names.insert(name);
        ASSERT_TRUE(fs::is_regular_file(hooks_json)) << hooks_json;
        EXPECT_EQ(
            acecode::sha256_hex(read_json(hooks_json).dump()),
            item["definition_sha256"].get<std::string>())
            << name;
    }
    for (const auto& seed : acecode::default_hook_seeds()) {
        EXPECT_EQ(manifest_hook_names.count(seed.name), 1u) << seed.name;
        auto item = std::find_if(
            manifest["hooks"].begin(),
            manifest["hooks"].end(),
            [&](const nlohmann::json& value) {
                return value.value("name", std::string{}) == seed.name;
            });
        ASSERT_NE(item, manifest["hooks"].end());
        EXPECT_EQ(
            (*item)["source_id"].get<std::string>(),
            seed.source_id);
        EXPECT_EQ(
            (*item)["relative_path"].get<std::string>(),
            seed.relative_path.generic_string());
        EXPECT_EQ(
            (*item)["definition_sha256"].get<std::string>(),
            seed.definition_sha256);
        ASSERT_TRUE((*item)["previous_definition_sha256s"].is_array());
        EXPECT_EQ(
            (*item)["previous_definition_sha256s"]
                .get<std::vector<std::string>>(),
            seed.previous_definition_sha256s);
    }
    ASSERT_EQ(manifest["hooks"].size(), 1u);
    const auto& packaged_hook = manifest["hooks"][0];
    EXPECT_EQ(
        read_json(
            seed_root / "hooks" /
            packaged_hook["relative_path"].get<std::string>() /
            "hooks.json"),
        read_json(repository_root / "docs" / "examples" /
                  "herdr-hooks.json"));
}

TEST(DefaultSkillSeedRegistryTest,
     HerdrHooksResolveCliWithoutOptionalBinPathAndKeepPaneIdentity) {
    const fs::path source_file = fs::absolute(fs::path(__FILE__));
    const fs::path repository_root =
        source_file.parent_path().parent_path().parent_path();
    const auto config = read_json(
        repository_root / "assets" / "seed" / "hooks" /
        "agent-reporting" / "hooks.json");
    const fs::path temp_root = make_temp_root("herdr-cli-fallback");
    const fs::path fake_bin = temp_root / "bin";
    const fs::path calls_log = temp_root / "calls.log";
    fs::create_directories(fake_bin);

#ifdef _WIN32
    const fs::path fake_herdr = fake_bin / "herdr.cmd";
    write_file(
        fake_herdr,
        "@echo off\r\n"
        ">>\"%HERDR_TEST_LOG%\" echo %~1^|%~3\r\n"
        "exit /b 0\r\n");
    constexpr const char* command_key = "commandWindows";
    constexpr const char path_separator = ';';
#else
    const fs::path fake_herdr = fake_bin / "herdr";
    write_file(
        fake_herdr,
        "#!/bin/sh\n"
        "printf '%s|%s\\n' \"$1\" \"$3\" >> \"$HERDR_TEST_LOG\"\n");
    fs::permissions(
        fake_herdr,
        fs::perms::owner_read | fs::perms::owner_write |
            fs::perms::owner_exec,
        fs::perm_options::replace);
    constexpr const char* command_key = "command";
    constexpr const char path_separator = ':';
#endif

    const std::string original_path =
        std::getenv("PATH") ? std::getenv("PATH") : "";
    const std::string test_path =
        fake_bin.string() + path_separator + original_path;
    const std::string expected_pane = "w-test:p-expected";
    const std::string expected_tab = "w-test:t-expected";
    ScopedEnvironmentValue herdr_env("HERDR_ENV", "1");
    ScopedEnvironmentValue pane_id("HERDR_PANE_ID", expected_pane);
    ScopedEnvironmentValue tab_id("HERDR_TAB_ID", expected_tab);
    ScopedEnvironmentValue socket_path(
        "HERDR_SOCKET_PATH", (temp_root / "herdr.sock").string());
    ScopedEnvironmentValue missing_bin_path("HERDR_BIN_PATH", std::nullopt);
    ScopedEnvironmentValue path("PATH", test_path);
    ScopedEnvironmentValue calls("HERDR_TEST_LOG", calls_log.string());
    ScopedEnvironmentValue title(
        "ACECODE_HOOK_SESSION_TITLE", "Seed title");
#ifdef _WIN32
    ScopedEnvironmentValue local_app_data(
        "LOCALAPPDATA", (temp_root / "missing-local-app-data").string());
#endif

    std::size_t handler_count = 0;
    for (const auto& event : config.at("hooks").items()) {
        for (const auto& group : event.value()) {
            for (const auto& handler : group.at("hooks")) {
                ++handler_count;
                const std::string command =
                    handler.at(command_key).get<std::string>();
                EXPECT_EQ(command.find("HERDR_ACTIVE_PANE_ID"), std::string::npos);
                EXPECT_EQ(command.find("pane current"), std::string::npos);
                auto result = acecode::run_hook_shell_command(
                    command, "{}", 3000, temp_root.string());
                EXPECT_TRUE(result.started) << event.key() << ": " << result.error;
                EXPECT_FALSE(result.timed_out) << event.key();
                EXPECT_EQ(result.exit_code, 0)
                    << event.key() << ": " << result.output;
            }
        }
    }

    EXPECT_EQ(handler_count, 9u);
    std::istringstream recorded(read_file(calls_log));
    std::vector<std::string> lines;
    for (std::string line; std::getline(recorded, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    ASSERT_EQ(lines.size(), handler_count);
    for (const auto& line : lines) {
        if (line.rfind("tab|", 0) == 0) {
            EXPECT_EQ(line, "tab|" + expected_tab);
        } else {
            EXPECT_EQ(line, "pane|" + expected_pane);
        }
    }

    std::error_code cleanup_error;
    fs::remove_all(temp_root, cleanup_error);
}

TEST(DefaultSkillSeedRegistryTest,
     HerdrTitleHookKeepsExactTabAndTitleAndSkipsEmptyOrMissingEnvironment) {
    const fs::path source_file = fs::absolute(fs::path(__FILE__));
    const fs::path repository_root =
        source_file.parent_path().parent_path().parent_path();
    const auto config = read_json(
        repository_root / "assets" / "seed" / "hooks" /
        "agent-reporting" / "hooks.json");
    const auto& handler = config.at("hooks")
                              .at("SessionTitleChanged")
                              .at(0)
                              .at("hooks")
                              .at(0);
    const fs::path temp_root = make_temp_root("herdr-title-hook");
    const fs::path fake_bin = temp_root / "bin";
    const fs::path calls_log = temp_root / "calls.log";
    fs::create_directories(fake_bin);

#ifdef _WIN32
    const fs::path fake_herdr = fake_bin / "herdr.cmd";
    write_file(
        fake_herdr,
        "@echo off\r\n"
        "set \"HERDR_CAPTURE_1=%~1\"\r\n"
        "set \"HERDR_CAPTURE_2=%~2\"\r\n"
        "set \"HERDR_CAPTURE_3=%~3\"\r\n"
        "set \"HERDR_CAPTURE_4=%~4\"\r\n"
        "powershell.exe -NoLogo -NoProfile -NonInteractive -Command \"$line=$env:HERDR_CAPTURE_1+'|'+$env:HERDR_CAPTURE_2+'|'+$env:HERDR_CAPTURE_3+'|'+$env:HERDR_CAPTURE_4+[char]10; [IO.File]::AppendAllText($env:HERDR_TEST_LOG,$line,[Text.UTF8Encoding]::new($false))\"\r\n"
        "exit /b 0\r\n");
    constexpr const char* command_key = "commandWindows";
    constexpr const char path_separator = ';';
#else
    const fs::path fake_herdr = fake_bin / "herdr";
    write_file(
        fake_herdr,
        "#!/bin/sh\n"
        "printf '%s|%s|%s|%s\\n' \"$1\" \"$2\" \"$3\" \"$4\" >> \"$HERDR_TEST_LOG\"\n");
    fs::permissions(
        fake_herdr,
        fs::perms::owner_read | fs::perms::owner_write |
            fs::perms::owner_exec,
        fs::perm_options::replace);
    constexpr const char* command_key = "command";
    constexpr const char path_separator = ':';
#endif

    const std::string original_path =
        std::getenv("PATH") ? std::getenv("PATH") : "";
    ScopedEnvironmentValue path(
        "PATH", fake_bin.string() + path_separator + original_path);
    ScopedEnvironmentValue herdr_env("HERDR_ENV", "1");
    ScopedEnvironmentValue tab_id("HERDR_TAB_ID", "w-test:t-title");
    ScopedEnvironmentValue socket_path(
        "HERDR_SOCKET_PATH", (temp_root / "herdr.sock").string());
    ScopedEnvironmentValue missing_bin_path("HERDR_BIN_PATH", std::nullopt);
    ScopedEnvironmentValue calls("HERDR_TEST_LOG", calls_log.string());
#ifdef _WIN32
    ScopedEnvironmentValue local_app_data(
        "LOCALAPPDATA", (temp_root / "missing-local-app-data").string());
#endif

    const std::string title = "构建 & 发布 $HOME";
    const std::string command = handler.at(command_key).get<std::string>();
    auto renamed = acecode::run_hook_shell_command(
        command,
        R"({"title":"ignored-stdin-copy"})",
        3000,
        temp_root.string(),
        {{"ACECODE_HOOK_SESSION_TITLE", title}});
    ASSERT_TRUE(renamed.started) << renamed.error;
    ASSERT_FALSE(renamed.timed_out);
    ASSERT_EQ(renamed.exit_code, 0) << renamed.output;
    EXPECT_EQ(
        read_file(calls_log),
        "tab|rename|w-test:t-title|" + title + "\n");

    auto empty = acecode::run_hook_shell_command(
        command,
        R"({"title":""})",
        3000,
        temp_root.string(),
        {{"ACECODE_HOOK_SESSION_TITLE", ""}});
    ASSERT_EQ(empty.exit_code, 0) << empty.output;
    EXPECT_EQ(
        read_file(calls_log),
        "tab|rename|w-test:t-title|" + title + "\n");

    {
        ScopedEnvironmentValue missing_tab("HERDR_TAB_ID", std::nullopt);
        auto missing = acecode::run_hook_shell_command(
            command,
            R"({"title":"missing tab"})",
            3000,
            temp_root.string(),
            {{"ACECODE_HOOK_SESSION_TITLE", "missing tab"}});
        ASSERT_EQ(missing.exit_code, 0) << missing.output;
    }
    EXPECT_EQ(
        read_file(calls_log),
        "tab|rename|w-test:t-title|" + title + "\n");

    std::error_code cleanup_error;
    fs::remove_all(temp_root, cleanup_error);
}

TEST(DefaultSkillSeedRegistryTest, PackagedResourcesInitializeACleanUserHome) {
    const fs::path source_file = fs::absolute(fs::path(__FILE__));
    const fs::path repository_root =
        source_file.parent_path().parent_path().parent_path();
    const fs::path seed_root = repository_root / "assets" / "seed";
    const fs::path temp_root = make_temp_root("packaged-resources");
    const fs::path home = temp_root / "profile" / ".acecode";

    const auto result = acecode::reconcile_default_global_skills(
        home, seed_root / "skills");

    EXPECT_TRUE(result.error.empty()) << result.error;
    ASSERT_TRUE(result.version_written);
    EXPECT_EQ(
        count_outcome(result, "installed"),
        acecode::default_skill_seeds().size());
    EXPECT_EQ(
        count_expert_outcome(result, "installed"),
        acecode::default_expert_seeds().size());
    EXPECT_EQ(
        count_hook_outcome(result, "installed"),
        acecode::default_hook_seeds().size());

    acecode::SkillRegistry skill_registry;
    skill_registry.set_scan_roots({home / "skills"});
    skill_registry.scan();
    EXPECT_TRUE(skill_registry.find("expert-manager").has_value());

    acecode::ExpertRegistry expert_registry(home / "experts");
    std::vector<acecode::ExpertDiagnostic> diagnostics;
    const auto experts =
        expert_registry.list(temp_root.string(), &diagnostics);
    EXPECT_TRUE(diagnostics.empty());
    EXPECT_EQ(experts.size(), acecode::default_expert_seeds().size());
    EXPECT_TRUE(
        expert_registry.find(temp_root.string(), "opc-team").has_value());

    acecode::HookLoadOptions hook_options;
    hook_options.acecode_home = home.string();
    hook_options.codex_home = (temp_root / "missing-codex").string();
    hook_options.include_project_sources = false;
    const auto hook_registry = acecode::load_hook_registry(hook_options);
    ASSERT_EQ(hook_registry.hooks.size(), 9u);
    for (const auto& hook : hook_registry.hooks) {
        EXPECT_TRUE(hook.managed);
        EXPECT_EQ(
            hook.trust_status,
            acecode::HookTrustStatus::ManagedTrusted);
    }

    const auto state =
        read_json(acecode::default_skill_seed_state_path(home));
    EXPECT_TRUE(state["completed"].get<bool>());
    EXPECT_EQ(state["bundle_version"], "2026-08-14.1");

    std::error_code cleanup_error;
    fs::remove_all(temp_root, cleanup_error);
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

TEST(DefaultSkillSeedRegistryTest, OpcExpertsAndManagerAreRegistered) {
    EXPECT_EQ(acecode::default_expert_seeds().size(), 10u);
    std::set<std::string> expert_names;
    for (const auto& seed : acecode::default_expert_seeds()) {
        expert_names.insert(seed.name);
        EXPECT_NE(
            seed.source_id.find("acecode:opc-expert/"),
            std::string::npos);
    }
    EXPECT_EQ(expert_names.count("opc-team"), 1u);
    EXPECT_EQ(expert_names.count("opc-team-lead"), 1u);

    bool manager_found = false;
    for (const auto& seed : acecode::default_skill_seeds()) {
        if (seed.name != "expert-manager") continue;
        manager_found = true;
        EXPECT_EQ(
            seed.relative_path.generic_string(),
            "expert-management/expert-manager");
    }
    EXPECT_TRUE(manager_found);
}

} // namespace
