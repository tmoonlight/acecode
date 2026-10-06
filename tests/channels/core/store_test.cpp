#include <gtest/gtest.h>

#include "channels/core/store.hpp"
#include "test_support/channels/test_support.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

// channels/core/store:每个平台目录下的 config.json(开关、凭据、机主、授权名单)
// 与 state.json(会话绑定、创建记录、去重回执、游标)。

namespace acecode::channels::core {
namespace {

im::Address telegram_user(const std::string& id) {
    im::Address address;
    address.platform = "telegram";
    address.account = "100";
    address.kind = im::ChatKind::Private;
    address.chat = id;
    address.sender = id;
    return address;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// 场景:设置页保存了凭据与授权名单,运行时写入了绑定、创建记录、回执和游标,然后进程重启。
// 期望:新的 ChannelStore 读盘后,所有字段与重启前一致。
TEST(ChannelStore, RoundTripsConfigAndStateAcrossRestart) {
    const auto dir = channels::test::temporary("store-roundtrip");
    {
        ChannelStore store(dir);
        store.load();
        store.update_config([](PlatformConfig& config) {
            config.enabled = true;
            config.credentials = {{"token", "100:secret-token"}};
            config.owner = "user:1";
            config.access.push_back({"user:1", "机主", 42});
            config.access.push_back({"group:-1001", "", 43});
        });
        BindingRecord record;
        record.address = telegram_user("1");
        record.session_id = "s-1";
        record.cwd = "C:/work/项目";
        record.workspace_hash = "abc";
        record.no_workspace = false;
        record.updated_at_ms = 7;
        store.put_binding(record);
        store.remember_created(record.address.key(), "s-1");
        store.remember_receipt(record.address.key(), "m-1");
        store.set_cursor("telegram_offset", 99);
    }
    ChannelStore reloaded(dir);
    reloaded.load();
    const auto config = reloaded.config();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.credentials.value("token", ""), "100:secret-token");
    EXPECT_EQ(config.owner, "user:1");
    ASSERT_EQ(config.access.size(), 2u);
    EXPECT_EQ(config.access[0].name, "机主");
    const auto key = telegram_user("1").key();
    const auto binding = reloaded.binding(key);
    ASSERT_TRUE(binding.has_value());
    EXPECT_EQ(binding->session_id, "s-1");
    EXPECT_EQ(binding->cwd, "C:/work/项目");
    EXPECT_FALSE(binding->no_workspace);
    EXPECT_EQ(reloaded.created_sessions(key), std::vector<std::string>{"s-1"});
    EXPECT_TRUE(reloaded.has_receipt(key, "m-1"));
    EXPECT_FALSE(reloaded.has_receipt(key, "m-2"));
    EXPECT_EQ(reloaded.cursor("telegram_offset"), 99);
    std::filesystem::remove_all(dir);
}

// 场景:config.json 被写坏(不是 JSON),或 state.json 里绑定的键与地址不一致。
// 期望:load() 抛出带中文原因的异常,拒绝启动该平台,且不会把文件重置成空配置。
TEST(ChannelStore, RejectsCorruptFilesWithoutResetting) {
    const auto dir = channels::test::temporary("store-corrupt");
    write_text(dir / "config.json", "{not json");
    ChannelStore broken(dir);
    try {
        broken.load();
        FAIL() << "损坏的配置应当被拒绝";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("损坏"), std::string::npos) << e.what();
    }
    {
        std::ifstream in(dir / "config.json", std::ios::binary);
        EXPECT_EQ(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), "{not json");
    }

    const auto dir2 = channels::test::temporary("store-bad-binding");
    nlohmann::json state{{"version", 1},
                         {"bindings", {{"wrong-key", {{"address", telegram_user("1").to_json()}, {"session_id", "s-1"}}}}},
                         {"created", nlohmann::json::object()},
                         {"receipts", nlohmann::json::array()},
                         {"cursors", nlohmann::json::object()}};
    write_text(dir2 / "state.json", state.dump());
    ChannelStore mismatched(dir2);
    EXPECT_THROW(mismatched.load(), std::runtime_error);

    write_text(dir2 / "state.json", R"({"version":2})");
    ChannelStore wrong_version(dir2);
    EXPECT_THROW(wrong_version.load(), std::runtime_error);
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(dir2);
}

// 场景:配置文件版本不对,或机主身份不是 user:/group:/member: 前缀。
// 期望:都视为无效配置,load() 抛异常。
TEST(ChannelStore, RejectsInvalidConfigStructure) {
    const auto dir = channels::test::temporary("store-bad-config");
    write_text(dir / "config.json", R"({"version":9,"enabled":true})");
    ChannelStore bad_version(dir);
    EXPECT_THROW(bad_version.load(), std::runtime_error);
    write_text(dir / "config.json", R"({"version":1,"enabled":true,"owner":"admin"})");
    ChannelStore bad_owner(dir);
    EXPECT_THROW(bad_owner.load(), std::runtime_error);
    write_text(dir / "config.json", R"({"version":1,"enabled":true,"credentials":{"token":5}})");
    ChannelStore bad_credentials(dir);
    EXPECT_THROW(bad_credentials.load(), std::runtime_error);
    std::filesystem::remove_all(dir);
}

// 场景:目录里还没有任何文件(首次使用)。
// 期望:load() 成功,平台处于关闭、无凭据、无机主的初始状态。
TEST(ChannelStore, MissingFilesMeanFreshPlatform) {
    const auto dir = channels::test::temporary("store-fresh");
    ChannelStore store(dir);
    store.load();
    const auto config = store.config();
    EXPECT_FALSE(config.enabled);
    EXPECT_TRUE(config.credentials.empty());
    EXPECT_TRUE(config.owner.empty());
    EXPECT_TRUE(store.bindings().empty());
    EXPECT_EQ(store.cursor("telegram_offset"), 0);
}

// 场景:设置页的多个操作(批准请求、撤销授权)在不同线程同时修改配置。
// 期望:每次修改都基于最新配置串行进行,8 个线程各加 25 条授权后共 200 条,重读一致。
TEST(ChannelStore, ConcurrentConfigEditsAreSerialized) {
    const auto dir = channels::test::temporary("store-concurrent");
    ChannelStore store(dir);
    store.load();
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 25; ++i) {
                store.update_config([&](PlatformConfig& config) {
                    config.access.push_back({"user:" + std::to_string(t * 100 + i), "", 0});
                });
            }
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(store.config().access.size(), 200u);
    ChannelStore reloaded(dir);
    reloaded.load();
    EXPECT_EQ(reloaded.config().access.size(), 200u);
    std::filesystem::remove_all(dir);
}

// 场景:一个 IM 会话通过 /new 反复建会话,超过每个 IM 会话保留的上限(200)。
// 期望:只保留最近 200 个,最早的被淘汰;重复登记同一个会话不产生重复项。
TEST(ChannelStore, CreatedSessionListIsBounded) {
    const auto dir = channels::test::temporary("store-created");
    ChannelStore store(dir);
    store.load();
    const auto key = telegram_user("1").key();
    for (int i = 0; i < 205; ++i) store.remember_created(key, "s-" + std::to_string(i));
    store.remember_created(key, "s-204");
    const auto created = store.created_sessions(key);
    ASSERT_EQ(created.size(), ChannelStore::kMaxCreatedPerConversation);
    EXPECT_EQ(created.front(), "s-5");
    EXPECT_EQ(created.back(), "s-204");
    std::filesystem::remove_all(dir);
}

// 场景:处理消息时先记回执,随后处理失败需要允许对方重发。
// 期望:forget_receipt 后同一条消息不再被视为已处理;其它消息的回执不受影响。
TEST(ChannelStore, ReceiptsCanBeForgottenAfterFailure) {
    const auto dir = channels::test::temporary("store-receipts");
    ChannelStore store(dir);
    store.load();
    const auto key = telegram_user("1").key();
    store.remember_receipt(key, "m-1");
    store.remember_receipt(key, "m-2");
    store.forget_receipt(key, "m-1");
    EXPECT_FALSE(store.has_receipt(key, "m-1"));
    EXPECT_TRUE(store.has_receipt(key, "m-2"));
    EXPECT_FALSE(store.has_receipt(telegram_user("2").key(), "m-2"));
    std::filesystem::remove_all(dir);
}

#ifndef _WIN32
// 场景:保存了含 token 的配置。
// 期望:config.json 只有当前用户可读写(0600)。
TEST(ChannelStore, ConfigFileIsPrivate) {
    const auto dir = channels::test::temporary("store-private");
    ChannelStore store(dir);
    store.load();
    store.update_config([](PlatformConfig& config) { config.credentials = {{"token", "100:secret"}}; });
    const auto perms = std::filesystem::status(dir / "config.json").permissions();
    using std::filesystem::perms;
    EXPECT_EQ(perms & (perms::group_all | perms::others_all), perms::none);
    std::filesystem::remove_all(dir);
}
#endif

} // namespace
} // namespace acecode::channels::core
