// 覆盖桌面版「启动时回到上次停留的会话」的持久化与落地 URL(src/apps/desktop/last_session.cpp)。
#include <gtest/gtest.h>

#include "desktop/last_session.hpp"
#include "test_support/utils/state_file_fixture.hpp"
#include "utils/state_file.hpp"
#include "workspace/workspace_registry.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using acecode::desktop::LastSession;
using acecode::desktop::handle_set_last_session;
using acecode::desktop::last_session_url;
using acecode::desktop::read_last_session;
using acecode::desktop::restorable_last_session;
using acecode::desktop::write_last_session;
using acecode::test_support::StateFileTest;
using acecode::test_support::write_raw;

namespace {

bool bridge_ok(const std::string& response) {
    const auto parsed = nlohmann::json::parse(response);
    return parsed.value("ok", false);
}

} // namespace

// 触发场景:用户停在某个工作区会话上退出,下次启动读回记录。
// 期望行为:会话 id、工作区 hash、只读标记原样往返;与 state.json 里别的 key 互不覆盖。
TEST_F(StateFileTest, LastSessionRoundTripKeepsOtherState) {
    EXPECT_FALSE(read_last_session().has_value()); // 初始没有记录

    acecode::write_state_flag("some_flag", true);
    write_last_session(LastSession{"20261116-abc", "ws-hash-1", false, true});
    const auto restored = read_last_session();
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(*restored, (LastSession{"20261116-abc", "ws-hash-1", false, true}));
    EXPECT_TRUE(acecode::read_state_flag("some_flag"));

    // 覆盖写:切到另一个会话。
    write_last_session(LastSession{"20261116-def", "ws-hash-2", false, false});
    EXPECT_EQ(read_last_session()->session_id, "20261116-def");
}

// 触发场景:用户从会话回到首页后退出(前端上报 null)。
// 期望行为:记录被清掉,下次启动落在首页,而不是更早停留过的那个会话。
TEST_F(StateFileTest, LastSessionClearedWhenUserLeavesTheSession) {
    write_last_session(LastSession{"s1", "ws-1", false, false});
    ASSERT_TRUE(read_last_session().has_value());

    write_last_session(std::nullopt);
    EXPECT_FALSE(read_last_session().has_value());
    EXPECT_FALSE(acecode::read_state_json().contains("last_session"));
}

// 触发场景:无工作区会话。
// 期望行为:no_workspace 往返保留,且不带 workspace_hash —— 带了的话启动时会按
// 工作区去 resume,daemon 回 404。
TEST_F(StateFileTest, LastSessionNoWorkspaceDropsWorkspaceHash) {
    ASSERT_TRUE(bridge_ok(handle_set_last_session(
        R"([{"session_id":"s9","workspace_hash":"stale","no_workspace":true}])")));
    const auto restored = read_last_session();
    ASSERT_TRUE(restored.has_value());
    EXPECT_TRUE(restored->no_workspace);
    EXPECT_TRUE(restored->workspace_hash.empty());
}

// 触发场景:state.json 被手改或被旧版本写坏(字段类型不对、会话 id 为空、带控制字符)。
// 期望行为:读成「没有记录」而不是抛异常或拼出畸形 URL。
TEST_F(StateFileTest, LastSessionMalformedRecordReadsEmpty) {
    write_raw(path_, R"({"last_session": "s1"})");
    EXPECT_FALSE(read_last_session().has_value());

    write_raw(path_, R"({"last_session": {"session_id": 42}})");
    EXPECT_FALSE(read_last_session().has_value());

    write_raw(path_, R"({"last_session": {"session_id": ""}})");
    EXPECT_FALSE(read_last_session().has_value());

    write_raw(path_, R"({"last_session": {"session_id": "a\nb"}})");
    EXPECT_FALSE(read_last_session().has_value());

    // 257 字节:超过 id 长度上限(256),不是一个会话 id。
    write_raw(path_, nlohmann::json{
        {"last_session", {{"session_id", std::string(257, 'a')}}}}.dump());
    EXPECT_FALSE(read_last_session().has_value());
}

// 触发场景:前端经 aceDesktop_setLastSession 上报;参数是 webview 传来的 JSON 数组。
// 期望行为:会话对象写入、null 清除;缺参数 / 非法内容 / 非 JSON 一律回 ok=false
// 且不动已有记录。
TEST_F(StateFileTest, LastSessionBridgeAcceptsSessionAndNull) {
    EXPECT_TRUE(bridge_ok(handle_set_last_session(
        R"([{"session_id":"s1","workspace_hash":"ws-1"}])")));
    ASSERT_TRUE(read_last_session().has_value());
    EXPECT_EQ(read_last_session()->workspace_hash, "ws-1");

    EXPECT_FALSE(bridge_ok(handle_set_last_session("[]")));
    EXPECT_FALSE(bridge_ok(handle_set_last_session(R"([{"workspace_hash":"ws-1"}])")));
    EXPECT_FALSE(bridge_ok(handle_set_last_session("not json")));
    ASSERT_TRUE(read_last_session().has_value());
    EXPECT_EQ(read_last_session()->session_id, "s1");

    EXPECT_TRUE(bridge_ok(handle_set_last_session("[null]")));
    EXPECT_FALSE(read_last_session().has_value());
}

// 触发场景:启动时把上次会话拼进 daemon 首页 URL。
// 期望行为:与跨 workspace 跳转同一套参数(open / workspace / no_workspace / read_only),
// 末尾带 restore=1 让前端静默处理失败;onboarding 的 about:blank 原样返回。
TEST(DesktopLastSessionUrl, AppendsLandingParameters) {
    const std::string base = "http://127.0.0.1:4100/?token=t";
    EXPECT_EQ(last_session_url(base, LastSession{"s1", "ws-1", false, false}),
              base + "&open=s1&workspace=ws-1&restore=1");
    EXPECT_EQ(last_session_url(base, LastSession{"s2", "", true, false}),
              base + "&open=s2&no_workspace=1&restore=1");
    EXPECT_EQ(last_session_url(base, LastSession{"s3", "ws-3", false, true}),
              base + "&open=s3&workspace=ws-3&read_only=1&restore=1");
    // 既没有工作区 hash 也没标记 no_workspace:两个参数都不带,由前端按会话 id 直接 resume。
    EXPECT_EQ(last_session_url(base, LastSession{"s4", "", false, false}),
              base + "&open=s4&restore=1");
    // 会话 id 里的保留字符必须 percent-encode,不能截断查询串。
    EXPECT_EQ(last_session_url("http://127.0.0.1:4100/", LastSession{"a&b=c", "", true, false}),
              "http://127.0.0.1:4100/?open=a%26b%3Dc&no_workspace=1&restore=1");
    EXPECT_EQ(last_session_url("about:blank", LastSession{"s1", "ws-1", false, false}),
              "about:blank");
}

// 触发场景:上次会话所在的工作区已被移除(registry 里没有),或它的目录被删。
// 期望行为:不恢复 —— daemon 无从 resume,硬拼参数只会让启动卡在「打开会话」遮罩上;
// 没有工作区信息的会话不受影响,交给前端去试。
TEST_F(StateFileTest, LastSessionRestoreRequiresAvailableWorkspace) {
    const fs::path root = fs::temp_directory_path() /
        ("acecode_last_session_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    const fs::path projects = root / "projects";
    const fs::path cwd = root / "repo";
    fs::create_directories(projects);
    fs::create_directories(cwd);

    acecode::desktop::WorkspaceRegistry registry;
    const auto workspace = registry.register_new(projects.string(), cwd.string());

    write_last_session(LastSession{"s1", workspace.hash, false, false});
    ASSERT_TRUE(restorable_last_session(registry).has_value());
    EXPECT_EQ(restorable_last_session(registry)->session_id, "s1");

    write_last_session(LastSession{"s1", "unknown-workspace-hash", false, false});
    EXPECT_FALSE(restorable_last_session(registry).has_value());

    write_last_session(LastSession{"s2", "", true, false});
    EXPECT_TRUE(restorable_last_session(registry).has_value());

    write_last_session(LastSession{"s1", workspace.hash, false, false});
    std::error_code ec;
    fs::remove_all(cwd, ec);
    EXPECT_FALSE(restorable_last_session(registry).has_value());

    fs::remove_all(root, ec);
}
