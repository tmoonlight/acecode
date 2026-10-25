#include <gtest/gtest.h>

#include "channels/core/access.hpp"
#include "channels/core/plugin_platforms.hpp"
#include "channels/core/platforms.hpp"
#include "test_support/channels/core_fakes.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>

// 外部平台插件清单(openspec add-channel-platform-plugins D1 / D2 / D4):解析、扫描、注册,
// 以及插件平台在 Address 校验和本人账号访问规则里的表现。

namespace acecode::channels::core {
namespace {

class PluginManifest : public ::testing::Test {
protected:
    void SetUp() override {
        root = channels::test::temporary("plugin-manifest");
        std::filesystem::create_directories(root / "example");
        std::ofstream(root / "example" / "plugin.exe", std::ios::binary) << "stub";
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void write(const std::string& dir, const nlohmann::json& manifest) {
        std::filesystem::create_directories(root / dir);
        std::ofstream(root / dir / kPluginManifestFile, std::ios::binary) << manifest.dump();
    }
    nlohmann::json valid() const {
        return {{"schema", kPluginManifestSchema},
                {"name", "example"},
                {"label", "Example IM"},
                {"command", "plugin.exe"},
                {"args", {"--channel-transport", "--cwd", "${manifest_dir}"}},
                {"account", "self"},
                {"features", {"watched_groups", "future-feature"}}};
    }
    std::filesystem::path root;
};

// 场景:连接器目录里一份完整的清单,command 是相对路径,args 里有 ${manifest_dir}。
// 期望:注册为插件平台:命令解析成清单目录下的绝对路径,占位符替换成清单目录,
// 账号模型为本人,声明的 watched_groups 生效、未知能力忽略。
TEST_F(PluginManifest, ParsesRelativeCommandAndManifestDirPlaceholder) {
    write("example", valid());
    const auto result = scan_plugin_platforms(root);
    ASSERT_TRUE(result.errors.empty()) << result.errors.front();
    ASSERT_EQ(result.platforms.size(), 1u);
    const auto& spec = result.platforms[0];
    EXPECT_EQ(spec.name, "example");
    EXPECT_EQ(spec.owner_binding, OwnerBinding::Self);
    EXPECT_TRUE(spec.watched_groups);
    ASSERT_TRUE(spec.plugin);
    EXPECT_EQ(path_from_utf8(spec.plugin->command), std::filesystem::absolute(root / "example" / "plugin.exe").lexically_normal());
    ASSERT_EQ(spec.plugin->args.size(), 3u);
    EXPECT_EQ(spec.plugin->args[2], spec.plugin->manifest_dir);
    EXPECT_TRUE(spec.fields.empty());
}

// 场景:名字与内置平台重名、schema 不对、程序不存在、账号模型未知、名字带大写。
// 期望:每份都整份跳过并给出原因;不影响同一目录里其它合法清单。
TEST_F(PluginManifest, InvalidManifestsAreSkippedWithReasons) {
    auto builtin = valid();
    builtin["name"] = "telegram";
    write("a-builtin", builtin);
    auto schema = valid();
    schema["schema"] = "acecode.channel-plugin.v1";
    write("b-schema", schema);
    auto missing = valid();
    missing["command"] = "missing.exe";
    write("c-missing", missing);
    auto account = valid();
    account["account"] = "bot";
    write("d-account", account);
    auto upper = valid();
    upper["name"] = "Example";
    write("e-upper", upper);
    write("example", valid());
    const auto result = scan_plugin_platforms(root);
    EXPECT_EQ(result.errors.size(), 5u);
    ASSERT_EQ(result.platforms.size(), 1u);
    EXPECT_EQ(result.platforms[0].name, "example");
}

// 场景:两个目录声明了同名插件。
// 期望:按目录名排序保留第一个,第二个记一条重名原因。
TEST_F(PluginManifest, DuplicateNamesKeepTheFirstDirectory) {
    std::filesystem::create_directories(root / "z-copy");
    std::ofstream(root / "z-copy" / "plugin.exe", std::ios::binary) << "stub";
    write("example", valid());
    write("z-copy", valid());
    const auto result = scan_plugin_platforms(root);
    ASSERT_EQ(result.platforms.size(), 1u);
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors[0].find("重名"), std::string::npos);
}

// 场景:注册插件平台后查平台表,再离开作用域。
// 期望:内置平台在前、插件在后;Address 接受插件平台名;作用域结束后插件消失。
// 回归保护:Address::valid 曾写死七个平台名,插件平台的绑定读盘会被判为损坏。
TEST_F(PluginManifest, RegistrationExtendsThePlatformTableUntilScopeEnds) {
    write("example", valid());
    {
        ScopedPluginPlatforms scoped(scan_plugin_platforms(root).platforms);
        ASSERT_NE(find_platform_spec("example"), nullptr);
        EXPECT_EQ(platform_names().front(), "qq");
        EXPECT_EQ(platform_names().back(), "example");
        EXPECT_TRUE(test::private_address("me", "example", "me").valid());
    }
    EXPECT_EQ(find_platform_spec("example"), nullptr);
    EXPECT_FALSE(test::private_address("me", "Bad_Name", "me").valid());
}

// 场景:本人账号模型的平台收到私聊:自己发给自己 / 别人发来;以及群消息。
// 期望:只有自己发给自己的放行并视为机主;其余一律忽略,不产生配对请求。
TEST_F(PluginManifest, SelfAccountAccessOnlyAllowsTheLoggedInAccount) {
    write("example", valid());
    ScopedPluginPlatforms scoped(scan_plugin_platforms(root).platforms);
    AccessControl access;
    PlatformConfig config;
    const auto own = access.evaluate(config, test::message(test::private_address("me", "example", "me"), "hi", "1"),
                                     AccessControl::Clock::now());
    EXPECT_EQ(own.decision, AccessDecision::Allow);
    EXPECT_TRUE(own.owner);
    const auto other = access.evaluate(
        config, test::message(test::private_address("u2", "example", "me"), "hi", "2"), AccessControl::Clock::now());
    EXPECT_EQ(other.decision, AccessDecision::Ignore);
    EXPECT_FALSE(other.created);
    EXPECT_TRUE(access.pending(AccessControl::Clock::now()).empty());
}

} // namespace
} // namespace acecode::channels::core
