#include <gtest/gtest.h>
#include "config/config.hpp"
#include "config/config_mutation.hpp"
#include "config/config_recovery.hpp"
#include "test_support/sandbox/test_support.hpp"
#include <fstream>
#include <future>
#include <iterator>
#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

std::string read_sandbox_config_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

} // namespace

// 场景:sandbox 段字段类型全错(字符串 / 数字 / null / 相对路径与空串)。
// 期望:逐字段静默回落默认值(enabled=true、不放行网络、无额外根),不阻塞启动。
TEST(ConfigSandbox, DefaultsAndInvalidFieldTypesRemainConservative) {
    sandbox::test::TempTree tree;
    auto path = tree.root / "config.json";
    tree.write(path, R"({"sandbox":{"enabled":"no","network_access":1,"exclude_tmpdir":null,
        "writable_roots":["relative",12,""]}})");
    auto config = load_config_from_path(path_to_utf8(path));
    EXPECT_TRUE(config.sandbox.enabled);
    EXPECT_FALSE(config.sandbox.network_access);
    EXPECT_FALSE(config.sandbox.exclude_tmpdir);
    EXPECT_TRUE(config.sandbox.writable_roots.empty());
}

// 场景:旧配置 `acceptEdits` + 非默认 sandbox 段。期望:模式归一成 auto;
// sandbox 四个字段 load → save → load 逐字段一致。
TEST(ConfigSandbox, RoundTripsSandboxAndNormalizesLegacyMode) {
    sandbox::test::TempTree tree;
    auto path = tree.root / "config.json";
    auto extra = path_to_utf8(tree.dir("extra"));
    tree.write(path, nlohmann::json{{"default_permission_mode", "acceptEdits"},
        {"sandbox", {{"enabled", false}, {"network_access", true},
                     {"exclude_tmpdir", true}, {"writable_roots", {extra}}}}}.dump());
    auto config = load_config_from_path(path_to_utf8(path));
    EXPECT_EQ(config.default_permission_mode, "auto");
    EXPECT_FALSE(config.sandbox.enabled);
    EXPECT_TRUE(config.sandbox.network_access);
    EXPECT_TRUE(config.sandbox.exclude_tmpdir);
    EXPECT_EQ(config.sandbox.writable_roots, std::vector<std::string>{extra});
    save_config(config, path_to_utf8(path));
    auto restored = load_config_from_path(path_to_utf8(path));
    EXPECT_EQ(restored.default_permission_mode, "auto");
    EXPECT_EQ(restored.sandbox.writable_roots, config.sandbox.writable_roots);
    EXPECT_EQ(restored.sandbox.enabled, config.sandbox.enabled);
    EXPECT_EQ(restored.sandbox.network_access, config.sandbox.network_access);
    EXPECT_EQ(restored.sandbox.exclude_tmpdir, config.sandbox.exclude_tmpdir);
}

// 场景:权限清单段(align-codex-sandboxing):filesystem.{read,write,deny} 条目原文
// (含 `~` 与 `:workspace_roots` 记号)、deny_defaults=false、windows_backend=mxc。
// 期望:条目原样保留(记号在策略构造时才展开),非字符串 / 空串跳过,非法
// windows_backend 值忽略;load → save → load 逐字段一致,默认值不落盘。
TEST(ConfigSandbox, RoundTripsFilesystemEntriesAndWindowsBackend) {
    sandbox::test::TempTree tree;
    auto path = tree.root / "config.json";
    tree.write(path, R"({"sandbox":{
        "filesystem":{"read":["~/.cache", ":workspace_roots/node_modules", "", 3],
                      "write":["/shared"], "deny":["~/.aws", "**/.env"]},
        "deny_defaults": false, "windows_backend": "mxc"}})");
    auto config = load_config_from_path(path_to_utf8(path));
    EXPECT_EQ(config.sandbox.filesystem_read, (std::vector<std::string>{"~/.cache", ":workspace_roots/node_modules"}));
    EXPECT_EQ(config.sandbox.filesystem_write, std::vector<std::string>{"/shared"});
    EXPECT_EQ(config.sandbox.filesystem_deny, (std::vector<std::string>{"~/.aws", "**/.env"}));
    EXPECT_FALSE(config.sandbox.deny_defaults);
    EXPECT_EQ(config.sandbox.windows_backend, "mxc");
    save_config(config, path_to_utf8(path));
    auto restored = load_config_from_path(path_to_utf8(path));
    EXPECT_EQ(restored.sandbox.filesystem_read, config.sandbox.filesystem_read);
    EXPECT_EQ(restored.sandbox.filesystem_write, config.sandbox.filesystem_write);
    EXPECT_EQ(restored.sandbox.filesystem_deny, config.sandbox.filesystem_deny);
    EXPECT_FALSE(restored.sandbox.deny_defaults);
    EXPECT_EQ(restored.sandbox.windows_backend, "mxc");
    tree.write(path, R"({"sandbox":{"windows_backend":"appcontainer"}})");
    auto invalid = load_config_from_path(path_to_utf8(path));
    EXPECT_TRUE(invalid.sandbox.windows_backend.empty());
    EXPECT_TRUE(invalid.sandbox.deny_defaults);
    save_config(invalid, path_to_utf8(path));
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("\"sandbox\""), std::string::npos) << "默认值不落盘";
}

TEST(ConfigSandboxMigration, DisablesLegacyValuesAndPreservesOtherJsonFields) {
    for (const auto& sandbox_json : {
             nlohmann::json(), nlohmann::json{{"enabled", true}},
             nlohmann::json{{"enabled", false}}}) {
        sandbox::test::TempTree tree;
        const auto path = tree.root / "config.json";
        nlohmann::json original = {
            {"max_sessions", 37}, {"default_permission_mode", "auto"},
            {"future_setting", {{"keep", "unchanged"}}},
            {"migrations", {{"other_migration", true}}}};
        if (!sandbox_json.is_null()) {
            original["sandbox"] = sandbox_json;
            original["sandbox"]["network_access"] = true;
            original["sandbox"]["filesystem"]["deny"] = {"~/.ssh", "**/.env"};
            original["sandbox"]["future_policy"] = "keep";
        }
        tree.write(path, original.dump());

        const auto migrated = disable_sandbox_once(path_to_utf8(path));
        ASSERT_TRUE(migrated.ok) << migrated.error;
        EXPECT_TRUE(migrated.changed);
        EXPECT_FALSE(migrated.config.sandbox.enabled);
        EXPECT_TRUE(migrated.config.sandbox_disable_migration_completed);
        original["sandbox"]["enabled"] = false;
        original["migrations"]["disable_sandbox_once"] = true;
        const auto bytes = read_sandbox_config_bytes(path);
        EXPECT_EQ(nlohmann::json::parse(bytes), original);
        EXPECT_EQ(read_last_good_config(path_to_utf8(path)),
                  std::optional<std::string>(bytes));

        const auto repeated = disable_sandbox_once(path_to_utf8(path));
        EXPECT_TRUE(repeated.ok) << repeated.error;
        EXPECT_FALSE(repeated.changed);
        EXPECT_EQ(read_sandbox_config_bytes(path), bytes);
    }
}

TEST(ConfigSandboxMigration, ReenabledSandboxSurvivesNormalSavesAndRepeatedMigration) {
    sandbox::test::TempTree tree;
    const auto path = tree.root / "config.json";
    tree.write(path, "{}");
    ASSERT_TRUE(disable_sandbox_once(path_to_utf8(path)).ok);
    auto config = load_config_from_path(path_to_utf8(path));
    ASSERT_TRUE(config.sandbox_disable_migration_completed);
    config.sandbox.enabled = true;
    save_config(config, path_to_utf8(path));

    // Saving an enabled sandbox omits its default-valued section. The separate
    // migration marker must still prevent a later startup from disabling it.
    EXPECT_FALSE(nlohmann::json::parse(read_sandbox_config_bytes(path)).contains("sandbox"));
    const auto other_setting = mutate_config([](AppConfig& candidate, std::string&) {
        candidate.max_sessions = 73;
        return true;
    }, path_to_utf8(path));
    ASSERT_TRUE(other_setting.ok) << other_setting.error;
    const auto bytes = read_sandbox_config_bytes(path);
    const auto repeated = disable_sandbox_once(path_to_utf8(path));
    ASSERT_TRUE(repeated.ok) << repeated.error;
    EXPECT_FALSE(repeated.changed);
    EXPECT_TRUE(repeated.config.sandbox.enabled);
    EXPECT_EQ(repeated.config.max_sessions, 73);
    EXPECT_EQ(read_sandbox_config_bytes(path), bytes);
}

TEST(ConfigSandboxMigration, CompletedMarkerPreservesExplicitEnabledAndDisabledChoices) {
    for (bool enabled : {true, false}) {
        sandbox::test::TempTree tree;
        const auto path = tree.root / "config.json";
        const auto bytes = nlohmann::json{
            {"sandbox", {{"enabled", enabled}}},
            {"migrations", {{"disable_sandbox_once", true}}}}.dump();
        tree.write(path, bytes);
        const auto repeated = disable_sandbox_once(path_to_utf8(path));
        ASSERT_TRUE(repeated.ok) << repeated.error;
        EXPECT_FALSE(repeated.changed);
        EXPECT_EQ(repeated.config.sandbox.enabled, enabled);
        EXPECT_EQ(read_sandbox_config_bytes(path), bytes);
    }
}

TEST(ConfigSandboxMigration, OnlyBooleanTrueCountsAsCompleted) {
    for (const auto& marker : {nlohmann::json(false), nlohmann::json("true"),
                              nlohmann::json(1), nlohmann::json(nullptr)}) {
        sandbox::test::TempTree tree;
        const auto path = tree.root / "config.json";
        tree.write(path, nlohmann::json{
            {"sandbox", {{"enabled", true}}},
            {"migrations", {{"disable_sandbox_once", marker}}}}.dump());
        const auto migrated = disable_sandbox_once(path_to_utf8(path));
        ASSERT_TRUE(migrated.ok) << migrated.error;
        EXPECT_TRUE(migrated.changed);
        EXPECT_FALSE(migrated.config.sandbox.enabled);
    }
}

TEST(ConfigSandboxMigration, FailedWriteLeavesOriginalIntactAndCanRetry) {
    sandbox::test::TempTree tree;
    const auto path = tree.root / "config.json";
    const auto blocker = tree.dir("config.json.tmp");
    const std::string original = R"({"sandbox":{"enabled":true}})";
    tree.write(path, original);
    const auto failed = disable_sandbox_once(path_to_utf8(path));
    EXPECT_FALSE(failed.ok);
    EXPECT_FALSE(failed.changed);
    EXPECT_EQ(failed.error_kind, ConfigMutationErrorKind::Persistence);
    EXPECT_EQ(read_sandbox_config_bytes(path), original);
    EXPECT_FALSE(load_config_from_path(path_to_utf8(path)).sandbox_disable_migration_completed);

    ASSERT_TRUE(std::filesystem::remove(blocker));
    const auto retried = disable_sandbox_once(path_to_utf8(path));
    ASSERT_TRUE(retried.ok) << retried.error;
    EXPECT_TRUE(retried.changed);
    EXPECT_FALSE(retried.config.sandbox.enabled);
}

TEST(ConfigSandboxMigration, ConcurrentAttemptsApplyExactlyOnce) {
    sandbox::test::TempTree tree;
    const auto path = path_to_utf8(tree.root / "config.json");
    tree.write(path_from_utf8(path), "{}");
    std::vector<std::future<ConfigMutationResult>> attempts;
    for (int i = 0; i < 8; ++i) {
        attempts.push_back(std::async(std::launch::async, [path] {
            return disable_sandbox_once(path);
        }));
    }
    int changed = 0;
    for (auto& attempt : attempts) {
        const auto result = attempt.get();
        EXPECT_TRUE(result.ok) << result.error;
        EXPECT_FALSE(result.config.sandbox.enabled);
        if (result.changed) ++changed;
    }
    EXPECT_EQ(changed, 1);
}
