// 本文件覆盖 desktop/agent_browser_cookie_jars 的纯逻辑(openspec
// add-agent-browser-cookie-modes):
//
// 1. 档位名解析与「未选过 / 未知值按共享」。
// 2. 存储键:三档下页面落到哪份存储,子代理归根会话,空 owner 归共享。
// 3. Windows Profile 名与 macOS 按会话 UUID 的派生(合法、稳定、互不相同)。
// 4. 切档计划、需要重建的页面、按根会话找页面。
// 5. 按会话存储第一次被用到时:先清空 / 从父会话复制 / 登记。

#include <gtest/gtest.h>

#include "desktop/agent_browser_cookie_jars.hpp"

#include <cctype>
#include <set>

using namespace acecode::desktop;

namespace {

AgentBrowserPageOwner owner_of(const std::string& session_id,
                               const std::string& root = {}) {
    AgentBrowserPageOwner owner;
    owner.session_id = session_id;
    owner.root_session_id = root;
    return owner;
}

AgentBrowserCookieStoreKey session_key(const std::string& id) {
    AgentBrowserCookieStoreKey key;
    key.kind = AgentBrowserCookieStoreKind::Session;
    key.session_id = id;
    return key;
}

// WebView2 Profile 名规则:最多 64 字符;只允许字母数字与 #@$()+-_~. 空格;不能以 . 或空格结尾。
bool valid_webview2_profile_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) continue;
        if (std::string("#@$()+-_~. ").find(c) == std::string::npos) return false;
    }
    return name.back() != '.' && name.back() != ' ';
}

} // namespace

// 触发场景:状态文件里存的档位原文 —— 三个规范名、从未选过(空)、未来版本写入的未知名。
// 期望行为:规范名按原义;空与未知一律按共享(Shared 是本项目默认,升级用户不掉登录)。
TEST(AgentBrowserCookieMode, StoredNamesResolveWithSharedAsDefault) {
    EXPECT_EQ(effective_agent_browser_cookie_mode("until_quit"),
              AgentBrowserCookieMode::UntilQuit);
    EXPECT_EQ(effective_agent_browser_cookie_mode("per_session"),
              AgentBrowserCookieMode::PerSession);
    EXPECT_EQ(effective_agent_browser_cookie_mode("shared"),
              AgentBrowserCookieMode::Shared);
    EXPECT_EQ(effective_agent_browser_cookie_mode(""), AgentBrowserCookieMode::Shared);
    EXPECT_EQ(effective_agent_browser_cookie_mode("per_workspace"),
              AgentBrowserCookieMode::Shared);
    EXPECT_FALSE(agent_browser_cookie_mode_from_name("Shared").has_value());
    for (const auto mode : {AgentBrowserCookieMode::UntilQuit,
                            AgentBrowserCookieMode::Shared,
                            AgentBrowserCookieMode::PerSession}) {
        EXPECT_EQ(agent_browser_cookie_mode_from_name(
                      agent_browser_cookie_mode_name(mode)),
                  mode);
    }
}

// 触发场景:同一个页面 owner 在三档下各落到哪份存储。
// 期望行为:退出即清 → until_quit;共享 → shared;按会话 → session:<根会话>,
// 子代理(root_session_id 指向父会话)用父会话那份,这样子代理能沿用父会话的登录。
TEST(AgentBrowserCookieStoreKey, ModesMapOwnersToStores) {
    const auto main_owner = owner_of("main");
    const auto child_owner = owner_of("child", "main");
    EXPECT_EQ(agent_browser_cookie_store_key(AgentBrowserCookieMode::UntilQuit, main_owner).id(),
              "until_quit");
    EXPECT_EQ(agent_browser_cookie_store_key(AgentBrowserCookieMode::Shared, main_owner).id(),
              "shared");
    EXPECT_EQ(agent_browser_cookie_store_key(AgentBrowserCookieMode::PerSession, main_owner).id(),
              "session:main");
    EXPECT_EQ(agent_browser_cookie_store_key(AgentBrowserCookieMode::PerSession, child_owner),
              session_key("main"));
}

// 触发场景:按会话档下,页面没有 owner(旧版 daemon 建的页、未绑定的页)或会话 id 非法。
// 期望行为:落到共享存储,而不是凭空造一份按会话存储(那份存储永远不会被删除)。
TEST(AgentBrowserCookieStoreKey, OwnerlessOrInvalidOwnerFallsBackToShared) {
    EXPECT_EQ(agent_browser_cookie_store_key(AgentBrowserCookieMode::PerSession,
                                             AgentBrowserPageOwner{}).id(),
              "shared");
    EXPECT_EQ(agent_browser_cookie_store_key(AgentBrowserCookieMode::PerSession,
                                             owner_of("bad/id")).id(),
              "shared");
}

// 触发场景:Windows 宿主为三种存储选 WebView2 Profile。
// 期望行为:共享 = 默认 Profile(名字为空,沿用升级前的那份登录数据);退出即清 = 固定名的
// InPrivate Profile;按会话 = s-<哈希>,即使会话 id 很长(headless 自定义 id 最长 64、owner
// 字段最长 128)也符合 WebView2 的命名规则,且不同会话互不相同、同一会话每次相同。
TEST(AgentBrowserWindowsProfile, ProfileNamesAreValidStableAndDistinct) {
    const auto shared = agent_browser_windows_profile(AgentBrowserCookieStoreKey{});
    EXPECT_TRUE(shared.name.empty());
    EXPECT_FALSE(shared.in_private);

    AgentBrowserCookieStoreKey until_quit;
    until_quit.kind = AgentBrowserCookieStoreKind::UntilQuit;
    const auto ephemeral = agent_browser_windows_profile(until_quit);
    EXPECT_EQ(ephemeral.name, kAgentBrowserUntilQuitProfileName);
    EXPECT_TRUE(ephemeral.in_private);
    EXPECT_TRUE(valid_webview2_profile_name(ephemeral.name));

    const std::string long_id(128, 'a');
    const auto per_session = agent_browser_windows_profile(session_key(long_id));
    EXPECT_FALSE(per_session.in_private);
    EXPECT_TRUE(valid_webview2_profile_name(per_session.name)) << per_session.name;
    EXPECT_EQ(per_session.name.size(), 26u);
    EXPECT_EQ(per_session.name, agent_browser_session_profile_name(long_id));
    EXPECT_NE(agent_browser_session_profile_name("20261020_101010_abcd"),
              agent_browser_session_profile_name("20261020_101010_abce"));
    // 大小写不同的会话 id 也要映射到不同 Profile(WebView2 Profile 名大小写不敏感,
    // 直接拿 id 当名字会让它们撞到同一份存储)。
    EXPECT_NE(agent_browser_session_profile_name("Abc"),
              agent_browser_session_profile_name("abc"));
    // 磁盘目录是 WebView2 运行时决定的 WV2Profile_<名字>(冒烟实测),启动删除与
    // 运行中删除都靠它定位;写错就只会删一个不存在的目录,cookie 原样留在磁盘上。
    EXPECT_EQ(agent_browser_session_profile_directory_name(long_id),
              "WV2Profile_" + per_session.name);
}

// 触发场景:macOS 宿主为按会话存储派生 dataStoreForIdentifier 的 UUID。
// 期望行为:标准 8-4-4-4-12 大写形态,版本位 8、变体位 10xx;同一会话恒定,不同会话不同。
TEST(AgentBrowserSessionStoreUuid, UuidIsWellFormedAndStable) {
    const std::string uuid = agent_browser_session_store_uuid("20261020_101010_abcd");
    ASSERT_EQ(uuid.size(), 36u);
    EXPECT_EQ(uuid[8], '-');
    EXPECT_EQ(uuid[13], '-');
    EXPECT_EQ(uuid[18], '-');
    EXPECT_EQ(uuid[23], '-');
    EXPECT_EQ(uuid[14], '8');
    EXPECT_NE(std::string("89AB").find(uuid[19]), std::string::npos);
    for (const char c : uuid) {
        if (c == '-') continue;
        EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(c)) || (c >= 'A' && c <= 'F'))
            << uuid;
    }
    EXPECT_EQ(uuid, agent_browser_session_store_uuid("20261020_101010_abcd"));
    std::set<std::string> distinct;
    for (int i = 0; i < 200; ++i) {
        distinct.insert(agent_browser_session_store_uuid("session-" + std::to_string(i)));
    }
    EXPECT_EQ(distinct.size(), 200u);
}

// 触发场景:用户在三档之间切换。
// 期望行为:切到退出即清 → 立即清空共享存储 + 给所有按会话存储打待清空标记;
// 共享 ↔ 按会话互切不清任何数据(规格:持久档之间切换保留登录);离开退出即清时释放它的存储。
TEST(AgentBrowserCookieModeSwitch, OnlyUntilQuitDeletesPersistedData) {
    const auto to_quit = plan_agent_browser_cookie_mode_switch(
        AgentBrowserCookieMode::Shared, AgentBrowserCookieMode::UntilQuit);
    EXPECT_TRUE(to_quit.changed);
    EXPECT_TRUE(to_quit.clear_shared_now);
    EXPECT_TRUE(to_quit.mark_session_stores_for_wipe);
    EXPECT_FALSE(to_quit.release_until_quit_store);

    const auto between = plan_agent_browser_cookie_mode_switch(
        AgentBrowserCookieMode::Shared, AgentBrowserCookieMode::PerSession);
    EXPECT_TRUE(between.changed);
    EXPECT_FALSE(between.clear_shared_now);
    EXPECT_FALSE(between.mark_session_stores_for_wipe);
    EXPECT_FALSE(between.release_until_quit_store);

    const auto from_quit = plan_agent_browser_cookie_mode_switch(
        AgentBrowserCookieMode::UntilQuit, AgentBrowserCookieMode::PerSession);
    EXPECT_TRUE(from_quit.release_until_quit_store);
    EXPECT_FALSE(from_quit.clear_shared_now);

    EXPECT_FALSE(plan_agent_browser_cookie_mode_switch(
                     AgentBrowserCookieMode::PerSession,
                     AgentBrowserCookieMode::PerSession).changed);
}

// 触发场景:切档时有若干页面开着:A 的主页面、A 的子代理页面、无 owner 的页面。
// 期望行为:共享 → 按会话时,有 owner 的页面要换存储而重建;无 owner 的页面在两档下都用
// 共享存储,不重建(规格:存储不变的页面不重新加载)。
TEST(AgentBrowserPagesToRebuild, OnlyPagesWhoseStoreChangesAreRebuilt) {
    std::vector<AgentBrowserPageStoreBinding> pages = {
        {"p-main", owner_of("A"), AgentBrowserCookieStoreKey{}},
        {"p-child", owner_of("A-child", "A"), AgentBrowserCookieStoreKey{}},
        {"p-orphan", AgentBrowserPageOwner{}, AgentBrowserCookieStoreKey{}},
    };
    EXPECT_EQ(agent_browser_pages_to_rebuild(pages, AgentBrowserCookieMode::PerSession),
              (std::vector<std::string>{"p-main", "p-child"}));
    EXPECT_TRUE(agent_browser_pages_to_rebuild(pages, AgentBrowserCookieMode::Shared).empty());
    EXPECT_EQ(agent_browser_pages_to_rebuild(pages, AgentBrowserCookieMode::UntilQuit).size(), 3u);

    EXPECT_EQ(agent_browser_pages_of_root_session(pages, "A"),
              (std::vector<std::string>{"p-main", "p-child"}));
    EXPECT_TRUE(agent_browser_pages_of_root_session(pages, "").empty());
}

// 触发场景:分叉会话 C(父 A)第一次打开浏览器,A 已经有自己的存储。
// 期望行为:从 A 复制、登记 C 的存储;写回后分叉记录被消费,下一次不会再复制。
TEST(AgentBrowserSessionStorePreparation, ForkSeedsFromParentOnlyOnce) {
    AgentBrowserStoreState state;
    state.session_stores = {"A"};
    state.pending_seeds.push_back({"C", "A", 1});

    const auto first = prepare_agent_browser_session_store("C", state);
    EXPECT_FALSE(first.wipe_first);
    EXPECT_EQ(first.seed_from, "A");
    EXPECT_TRUE(first.register_store);

    apply_agent_browser_session_store_preparation("C", state);
    EXPECT_TRUE(state.pending_seeds.empty());
    EXPECT_EQ(state.session_stores, (std::vector<std::string>{"A", "C"}));

    const auto again = prepare_agent_browser_session_store("C", state);
    EXPECT_TRUE(again.seed_from.empty());
    EXPECT_FALSE(again.register_store);
}

// 触发场景:父会话没有存储、父会话存储待清空 / 待删除,或子会话已经有自己的存储。
// 期望行为:都不复制(规格:父会话没有存储时子会话从空开始;已有存储的不再复制),
// 且不能从已经被要求清空 / 删除的存储里把登录态带出来。
TEST(AgentBrowserSessionStorePreparation, SeedSkipsUnusableParentsAndExistingStores) {
    AgentBrowserStoreState state;
    state.pending_seeds.push_back({"C1", "missing", 1});
    state.session_stores = {"wiped", "deleted", "C4"};
    state.pending_wipe = {"wiped"};
    state.pending_deletions = {"deleted"};
    state.pending_seeds.push_back({"C2", "wiped", 2});
    state.pending_seeds.push_back({"C3", "deleted", 3});
    state.pending_seeds.push_back({"C4", "deleted", 4});
    EXPECT_TRUE(prepare_agent_browser_session_store("C1", state).seed_from.empty());
    EXPECT_TRUE(prepare_agent_browser_session_store("C2", state).seed_from.empty());
    EXPECT_TRUE(prepare_agent_browser_session_store("C3", state).seed_from.empty());
    EXPECT_TRUE(prepare_agent_browser_session_store("C4", state).seed_from.empty());
}

// 触发场景:切到退出即清后又切回按会话,会话 A 的存储在待清空清单里。
// 期望行为:先清空再用,用过之后移出待清空清单 —— 这样切回来时 A 不会带着切走前的登录。
TEST(AgentBrowserSessionStorePreparation, PendingWipeIsClearedBeforeReuse) {
    AgentBrowserStoreState state;
    state.session_stores = {"A"};
    state.pending_wipe = {"A"};
    const auto preparation = prepare_agent_browser_session_store("A", state);
    EXPECT_TRUE(preparation.wipe_first);
    EXPECT_FALSE(preparation.register_store);
    apply_agent_browser_session_store_preparation("A", state);
    EXPECT_TRUE(state.pending_wipe.empty());
    EXPECT_EQ(state.session_stores, (std::vector<std::string>{"A"}));
}
