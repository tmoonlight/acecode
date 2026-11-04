#include <gtest/gtest.h>

#include "tool/tool_rewrites.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir() {
    static std::atomic<unsigned int> sequence{0};
    auto path = fs::temp_directory_path() /
        ("acecode_tool_rewrites_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
         std::to_string(sequence.fetch_add(1)));
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}

struct TempDir {
    fs::path path = make_temp_dir();
    ~TempDir() { fs::remove_all(path); }
    std::string utf8() const { return acecode::path_to_utf8(path); }
};

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string mapping_of(const acecode::ToolProtocolNameMappings& mappings,
                       const std::string& native) {
    for (const auto& mapping : mappings) {
        if (mapping.native_name == native) return mapping.public_name;
    }
    return {};
}

} // namespace

// 场景:数据目录里还没有 tool-rewrites.json(全新安装 / 升级上来)。
// 期望:enabled=false、rewrites 预填内置种子,且无 error —— 用户第一次勾选时
// 右列已经有可用默认值,而进程生效映射为空(不重写)。
TEST(ToolRewrites, MissingFileYieldsDisabledSeedWithoutError) {
    TempDir dir;
    std::string error = "sentinel";
    const auto settings = acecode::tool_rewrites::load_settings(
        acecode::tool_rewrites::settings_path(dir.utf8()), &error);
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(settings.enabled);
    EXPECT_FALSE(settings.write_file_default_applied);
    EXPECT_EQ(mapping_of(settings.rewrites, "file_read"), "read");
    EXPECT_EQ(mapping_of(settings.rewrites, "TodoWrite"), "todowrite");
    EXPECT_TRUE(acecode::tool_rewrites::effective_mappings(settings).empty());
}

// 场景:解析磁盘格式。
// 期望:enabled / rewrites 正常读入;空值与「改名成自己」的 no-op 条目被丢弃;
// 未知字段忽略;非对象 / 非布尔 / 非字符串目标一律拒绝。
TEST(ToolRewrites, ParsesFileFormatAndDropsNoOpEntries) {
    acecode::tool_rewrites::ToolRewriteSettings settings;
    std::string error;
    ASSERT_TRUE(acecode::tool_rewrites::parse_settings(
        R"({"version":1,"enabled":true,"future":42,
            "rewrites":{"file_read":"read","bash":"","grep":"grep","file_write":" write "}})",
        settings, &error)) << error;
    EXPECT_TRUE(settings.enabled);
    EXPECT_FALSE(settings.write_file_default_applied);
    ASSERT_EQ(settings.rewrites.size(), 2u);
    EXPECT_EQ(mapping_of(settings.rewrites, "file_read"), "read");
    EXPECT_EQ(mapping_of(settings.rewrites, "file_write"), "write");
    EXPECT_EQ(mapping_of(settings.rewrites, "bash"), "");

    EXPECT_FALSE(acecode::tool_rewrites::parse_settings("[]", settings, &error));
    EXPECT_FALSE(acecode::tool_rewrites::parse_settings("not json", settings, &error));
    EXPECT_FALSE(acecode::tool_rewrites::parse_settings(
        R"({"enabled":"yes"})", settings, &error));
    EXPECT_NE(error.find("'enabled'"), std::string::npos);
    EXPECT_FALSE(acecode::tool_rewrites::parse_settings(
        R"({"rewrites":[]})", settings, &error));
    EXPECT_FALSE(acecode::tool_rewrites::parse_settings(
        R"({"rewrites":{"file_read":1}})", settings, &error));
    EXPECT_NE(error.find("file_read"), std::string::npos);

    ASSERT_TRUE(acecode::tool_rewrites::parse_settings(
        R"({"write_file_default_applied":true})", settings, &error)) << error;
    EXPECT_TRUE(settings.write_file_default_applied);
    EXPECT_FALSE(acecode::tool_rewrites::parse_settings(
        R"({"write_file_default_applied":"true"})", settings, &error));
    EXPECT_NE(error.find("write_file_default_applied"), std::string::npos);
}

// 场景:保存后重新读取。
// 期望:版本号 + enabled + rewrites 逐条往返一致;文件是 tmp+rename 原子写的
// 产物(没有残留 .tmp)。
TEST(ToolRewrites, SaveThenLoadRoundTrips) {
    TempDir dir;
    const std::string path = acecode::tool_rewrites::settings_path(dir.utf8());
    acecode::tool_rewrites::ToolRewriteSettings settings;
    settings.enabled = true;
    settings.rewrites = {{"file_read", "peek"}, {"bash", "shell"}};
    std::string error;
    ASSERT_TRUE(acecode::tool_rewrites::save_settings(path, settings, &error)) << error;
    EXPECT_TRUE(fs::exists(acecode::path_from_utf8(path)));
    EXPECT_FALSE(fs::exists(acecode::path_from_utf8(path + ".tmp")));

    const auto loaded = acecode::tool_rewrites::load_settings(path, &error);
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_TRUE(loaded.enabled);
    EXPECT_TRUE(loaded.write_file_default_applied);
    ASSERT_EQ(loaded.rewrites.size(), 2u);
    EXPECT_EQ(mapping_of(loaded.rewrites, "file_read"), "peek");
    EXPECT_EQ(mapping_of(loaded.rewrites, "bash"), "shell");

    const std::string text = acecode::tool_rewrites::serialize_settings(loaded);
    EXPECT_NE(text.find("\"version\": 1"), std::string::npos);
    EXPECT_NE(text.find("\"enabled\": true"), std::string::npos);
}

// 场景:非法映射(public 名撞另一条 native 名)想落盘。
// 期望:save 拒绝且不写文件;文件里若已有非法内容,load 回退默认并报 error。
TEST(ToolRewrites, RejectsInvalidMappingsOnSaveAndOnLoad) {
    TempDir dir;
    const std::string path = acecode::tool_rewrites::settings_path(dir.utf8());
    acecode::tool_rewrites::ToolRewriteSettings bad;
    bad.enabled = true;
    bad.rewrites = {{"file_read", "file_write"}, {"file_write", "write"}};
    std::string error;
    EXPECT_FALSE(acecode::tool_rewrites::save_settings(path, bad, &error));
    EXPECT_NE(error.find("collides"), std::string::npos);
    EXPECT_FALSE(fs::exists(acecode::path_from_utf8(path)));

    write_text(acecode::path_from_utf8(path),
               R"({"enabled":true,"rewrites":{"file_read":"bad name"}})");
    const auto loaded = acecode::tool_rewrites::load_settings(path, &error);
    EXPECT_NE(error.find("must match"), std::string::npos);
    EXPECT_FALSE(loaded.enabled);
    EXPECT_EQ(mapping_of(loaded.rewrites, "file_read"), "read");

    write_text(acecode::path_from_utf8(path), "{ broken");
    const auto broken = acecode::tool_rewrites::load_settings(path, &error);
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(broken.enabled);
}

// 场景:public 名与注册表里另一个真实工具同名(把 file_read 改叫 bash)。
// 期望:结合注册表的校验拒绝 —— 模型说 bash 时 resolve 会先命中真 bash,
// 重写目标永远到不了;把工具改成自己原名以外的未注册名则放行。
TEST(ToolRewrites, ValidatesAgainstRegisteredToolNames) {
    acecode::tool_rewrites::ToolRewriteSettings settings;
    settings.rewrites = {{"file_read", "bash"}};
    std::string error;
    EXPECT_FALSE(acecode::tool_rewrites::validate_settings_against_tools(
        settings, {"file_read", "bash"}, &error));
    EXPECT_NE(error.find("already the name of another tool"), std::string::npos);

    settings.rewrites = {{"file_read", "peek"}};
    EXPECT_TRUE(acecode::tool_rewrites::validate_settings_against_tools(
        settings, {"file_read", "bash"}, &error)) << error;
}

// 场景:启动加载 —— 文件启用了两条重写。
// 期望:进程生效映射与文件一致;同一目录改成 enabled=false 再加载,映射清空。
// 回归:load_and_apply 是 TUI / daemon / headless 三个入口共用的唯一发布点。
TEST(ToolRewrites, LoadAndApplyPublishesEffectiveMappingToProcess) {
    acecode::ScopedModelToolNameMappings restore(acecode::model_tool_name_mappings());
    TempDir dir;
    const std::string path = acecode::tool_rewrites::settings_path(dir.utf8());
    write_text(acecode::path_from_utf8(path),
               R"({"enabled":true,"rewrites":{"file_read":"peek","file_edit":"patch"}})");
    const auto applied = acecode::tool_rewrites::load_and_apply(dir.utf8());
    EXPECT_TRUE(applied.enabled);
    EXPECT_EQ(acecode::model_tool_name_for_native("file_read"), "peek");
    EXPECT_EQ(acecode::native_tool_name_for_public_alias("patch").value_or(""),
              "file_edit");

    acecode::tool_rewrites::ToolRewriteSettings disabled;
    disabled.rewrites = {{"file_read", "peek"}};
    ASSERT_TRUE(acecode::tool_rewrites::save_settings(path, disabled));
    acecode::tool_rewrites::load_and_apply(dir.utf8());
    EXPECT_TRUE(acecode::model_tool_name_mappings().empty());
    EXPECT_EQ(acecode::model_tool_name_for_native("file_read"), "file_read");

    // 文件损坏:保持不重写,不抛不崩。
    write_text(acecode::path_from_utf8(path), "{{{");
    acecode::ScopedModelToolNameMappings dirty(
        acecode::ToolProtocolNameMappings{{"bash", "shell"}});
    acecode::tool_rewrites::load_and_apply(dir.utf8());
    EXPECT_TRUE(acecode::model_tool_name_mappings().empty());
}

TEST(ToolRewrites, FirstStartupMigratesMissingFileAndPersistsCompletion) {
    acecode::ScopedModelToolNameMappings restore(acecode::model_tool_name_mappings());
    TempDir dir;
    const auto path = acecode::path_from_utf8(acecode::tool_rewrites::settings_path(dir.utf8()));

    const auto applied = acecode::tool_rewrites::load_and_apply(dir.utf8());
    EXPECT_TRUE(applied.enabled);
    EXPECT_TRUE(applied.write_file_default_applied);
    EXPECT_EQ(mapping_of(applied.rewrites, "file_write"), "write_file");
    EXPECT_EQ(mapping_of(applied.rewrites, "file_read"), "read");
    EXPECT_EQ(mapping_of(applied.rewrites, "file_edit"), "edit");
    EXPECT_EQ(mapping_of(applied.rewrites, "TodoWrite"), "todowrite");
    EXPECT_EQ(acecode::model_tool_name_for_native("file_write"), "write_file");
    EXPECT_EQ(acecode::native_tool_name_for_public_alias("write_file").value_or(""),
              "file_write");

    std::string error;
    const auto saved = acecode::tool_rewrites::load_settings(acecode::path_to_utf8(path), &error);
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_TRUE(saved.enabled);
    EXPECT_TRUE(saved.write_file_default_applied);
    EXPECT_EQ(mapping_of(saved.rewrites, "file_write"), "write_file");
    // Formatting is deliberately different: another startup must not rewrite the file.
    const auto persisted_text = read_text(path) + "\n";
    write_text(path, persisted_text);
    acecode::tool_rewrites::load_and_apply(dir.utf8());
    EXPECT_EQ(read_text(path), persisted_text);
}

TEST(ToolRewrites, DisabledMigrationUpsertsWriteFileAndRetainsOtherMappings) {
    acecode::ScopedModelToolNameMappings restore(acecode::model_tool_name_mappings());
    for (const std::string previous_write : {"", "write", "custom_write"}) {
        SCOPED_TRACE(previous_write);
        TempDir dir;
        const auto path = acecode::tool_rewrites::settings_path(dir.utf8());
        acecode::tool_rewrites::ToolRewriteSettings original;
        original.rewrites = {{"file_read", "peek"}, {"bash", "shell"}};
        if (!previous_write.empty()) original.rewrites.push_back({"file_write", previous_write});
        write_text(acecode::path_from_utf8(path), acecode::tool_rewrites::serialize_settings(original));

        const auto applied = acecode::tool_rewrites::load_and_apply(dir.utf8());
        EXPECT_TRUE(applied.enabled);
        EXPECT_TRUE(applied.write_file_default_applied);
        EXPECT_EQ(applied.rewrites.size(), 3u);
        EXPECT_EQ(mapping_of(applied.rewrites, "file_write"), "write_file");
        EXPECT_EQ(mapping_of(applied.rewrites, "file_read"), "peek");
        EXPECT_EQ(mapping_of(applied.rewrites, "bash"), "shell");
    }
}

TEST(ToolRewrites, AlreadyEnabledSettingsAreMarkedWithoutChangingMappings) {
    acecode::ScopedModelToolNameMappings restore(acecode::model_tool_name_mappings());
    for (const std::string previous_write : {"", "custom_write"}) {
        SCOPED_TRACE(previous_write);
        TempDir dir;
        const auto path = acecode::tool_rewrites::settings_path(dir.utf8());
        acecode::tool_rewrites::ToolRewriteSettings original;
        original.enabled = true;
        if (!previous_write.empty()) original.rewrites.push_back({"file_write", previous_write});
        write_text(acecode::path_from_utf8(path), acecode::tool_rewrites::serialize_settings(original));

        auto applied = acecode::tool_rewrites::load_and_apply(dir.utf8());
        EXPECT_TRUE(applied.enabled);
        EXPECT_TRUE(applied.write_file_default_applied);
        EXPECT_EQ(applied.rewrites.size(), original.rewrites.size());
        EXPECT_EQ(mapping_of(applied.rewrites, "file_write"), previous_write);
        EXPECT_TRUE(acecode::tool_rewrites::load_settings(path).write_file_default_applied);

        // A manual edit of only enabled preserves completion, just like a settings save.
        applied.enabled = false;
        write_text(acecode::path_from_utf8(path), acecode::tool_rewrites::serialize_settings(applied));
        EXPECT_FALSE(acecode::tool_rewrites::load_and_apply(dir.utf8()).enabled);
        EXPECT_TRUE(acecode::model_tool_name_mappings().empty());
    }
}

TEST(ToolRewrites, ExplicitSavesPreserveUserChoiceWithoutClientMigrationMetadata) {
    acecode::ScopedModelToolNameMappings restore(acecode::model_tool_name_mappings());
    TempDir dir;
    const auto path = acecode::tool_rewrites::settings_path(dir.utf8());
    acecode::tool_rewrites::load_and_apply(dir.utf8());

    for (const std::string body : {
             R"({"enabled":false,"rewrites":{"file_write":"write_file"}})",
             R"({"enabled":true,"rewrites":{"file_write":"my_write"}})",
             R"({"enabled":true,"rewrites":{},"write_file_default_applied":false})"}) {
        SCOPED_TRACE(body);
        acecode::tool_rewrites::ToolRewriteSettings edited;
        std::string error;
        ASSERT_TRUE(acecode::tool_rewrites::parse_settings(body, edited, &error)) << error;
        ASSERT_FALSE(edited.write_file_default_applied);
        ASSERT_TRUE(acecode::tool_rewrites::save_settings(path, edited, &error)) << error;
        const auto restarted = acecode::tool_rewrites::load_and_apply(dir.utf8());
        EXPECT_TRUE(restarted.write_file_default_applied);
        EXPECT_EQ(restarted.enabled, edited.enabled);
        EXPECT_EQ(restarted.rewrites.size(), edited.rewrites.size());
        EXPECT_EQ(mapping_of(restarted.rewrites, "file_write"), mapping_of(edited.rewrites, "file_write"));
        EXPECT_EQ(acecode::model_tool_name_for_native("file_write"),
                  edited.enabled && !edited.rewrites.empty() ? "my_write" : "file_write");
    }
}

TEST(ToolRewrites, ExplicitDisableBeforeFirstStartupIsRespected) {
    acecode::ScopedModelToolNameMappings restore(acecode::model_tool_name_mappings());
    TempDir dir;
    const auto path = acecode::tool_rewrites::settings_path(dir.utf8());
    ASSERT_TRUE(acecode::tool_rewrites::save_settings(path, acecode::tool_rewrites::default_settings()));
    EXPECT_FALSE(acecode::tool_rewrites::load_and_apply(dir.utf8()).enabled);
    EXPECT_TRUE(acecode::model_tool_name_mappings().empty());
}

TEST(ToolRewrites, MigrationWriteFailureDoesNotPublishAndCanRetry) {
    acecode::ScopedModelToolNameMappings restore(acecode::model_tool_name_mappings());
    for (const bool existing : {false, true}) {
        SCOPED_TRACE(existing);
        TempDir dir;
        const auto path = acecode::tool_rewrites::settings_path(dir.utf8());
        const std::string original = R"({"enabled":false,"rewrites":{"file_write":"write"}})";
        if (existing) write_text(acecode::path_from_utf8(path), original);
        const auto blocked_tmp = acecode::path_from_utf8(path + ".tmp");
        ASSERT_TRUE(fs::create_directory(blocked_tmp));

        const auto failed = acecode::tool_rewrites::load_and_apply(dir.utf8());
        EXPECT_FALSE(failed.enabled);
        EXPECT_FALSE(failed.write_file_default_applied);
        EXPECT_TRUE(acecode::model_tool_name_mappings().empty());
        if (existing) EXPECT_EQ(read_text(acecode::path_from_utf8(path)), original);
        else EXPECT_FALSE(fs::exists(acecode::path_from_utf8(path)));

        ASSERT_TRUE(fs::remove(blocked_tmp));
        const auto retried = acecode::tool_rewrites::load_and_apply(dir.utf8());
        EXPECT_TRUE(retried.enabled);
        EXPECT_TRUE(retried.write_file_default_applied);
        EXPECT_EQ(acecode::model_tool_name_for_native("file_write"), "write_file");
    }
}

TEST(ToolRewrites, MigrationPreservesBrokenOrConflictingSettings) {
    acecode::ScopedModelToolNameMappings restore(acecode::model_tool_name_mappings());
    TempDir dir;
    const auto path = acecode::path_from_utf8(acecode::tool_rewrites::settings_path(dir.utf8()));
    for (const std::string original : {
             "{{{",
             R"({"enabled":false,"write_file_default_applied":42})",
             R"({"enabled":true,"rewrites":{"file_write":"bad name"}})",
             R"({"enabled":false,"rewrites":{"file_read":"write_file"}})"}) {
        SCOPED_TRACE(original);
        write_text(path, original);
        const auto applied = acecode::tool_rewrites::load_and_apply(dir.utf8());
        EXPECT_FALSE(applied.enabled);
        EXPECT_FALSE(applied.write_file_default_applied);
        EXPECT_TRUE(acecode::model_tool_name_mappings().empty());
        EXPECT_EQ(read_text(path), original);
    }
}

TEST(ToolRewrites, NonFileSettingsPathIsNotTreatedAsMissing) {
    acecode::ScopedModelToolNameMappings restore(acecode::model_tool_name_mappings());
    TempDir dir;
    const auto path = acecode::tool_rewrites::settings_path(dir.utf8());
    ASSERT_TRUE(fs::create_directory(acecode::path_from_utf8(path)));
    std::string error;
    acecode::tool_rewrites::load_settings(path, &error);
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(acecode::tool_rewrites::load_and_apply(dir.utf8()).enabled);
    EXPECT_TRUE(fs::is_directory(acecode::path_from_utf8(path)));
    EXPECT_FALSE(fs::exists(acecode::path_from_utf8(path + ".tmp")));
}

TEST(ToolRewrites, MigrationUsesExistingLegacyFilename) {
    acecode::ScopedModelToolNameMappings restore(acecode::model_tool_name_mappings());
    TempDir dir;
    const auto legacy_path = dir.path / "tool-rewrites.json";
    write_text(legacy_path, R"({"enabled":false,"rewrites":{}})");
    const auto applied = acecode::tool_rewrites::load_and_apply(dir.utf8());
    EXPECT_TRUE(applied.enabled);
    EXPECT_EQ(applied.rewrites.size(), 1u);
    EXPECT_EQ(mapping_of(applied.rewrites, "file_write"), "write_file");
    EXPECT_EQ(acecode::tool_rewrites::settings_path(dir.utf8()), acecode::path_to_utf8(legacy_path));
    EXPECT_TRUE(acecode::tool_rewrites::load_settings(acecode::path_to_utf8(legacy_path)).write_file_default_applied);
    EXPECT_FALSE(fs::exists(dir.path / "tool_rewrites.json"));
}
