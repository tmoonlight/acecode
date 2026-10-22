// Agent Browser cookie 存储状态(state.json 的 agent_browser 段)的读写与规范化。
// openspec add-agent-browser-cookie-modes D6:Desktop 与 daemon 共用这一份格式。

#include <gtest/gtest.h>

#include "ipc/agent_browser_store_state.hpp"
#include "test_support/utils/state_file_fixture.hpp"
#include "utils/state_file.hpp"

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

using namespace acecode::desktop;
using acecode::test_support::StateFileTest;
using acecode::test_support::write_raw;

namespace {

class AgentBrowserStoreStateTest : public StateFileTest {};

nlohmann::json read_file_json(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return nlohmann::json::parse(in, nullptr, false);
}

} // namespace

// 场景:全新安装,state.json 不存在。
// 期望:读出空状态 —— 没选过档位(cookie_mode 为空,由使用方按 shared 处理),各清单为空。
TEST_F(AgentBrowserStoreStateTest, MissingFileReadsAsEmptyState) {
    const auto state = read_agent_browser_store_state();
    EXPECT_TRUE(state.cookie_mode.empty());
    EXPECT_TRUE(state.session_stores.empty());
    EXPECT_TRUE(state.pending_wipe.empty());
    EXPECT_FALSE(state.shared_wipe_pending);
    EXPECT_TRUE(state.pending_deletions.empty());
    EXPECT_TRUE(state.pending_seeds.empty());
}

// 场景:Desktop 写入完整状态后再读回。
// 期望:每个字段原样往返;空字段不写进文件(state.json 写入保持稀疏)。
TEST_F(AgentBrowserStoreStateTest, RoundTripKeepsFieldsAndOmitsEmptyOnes) {
    ASSERT_TRUE(update_agent_browser_store_state([](AgentBrowserStoreState& state) {
        state.cookie_mode = "per_session";
        state.session_stores = {"20261020_101010_abcd", "headless-run-1"};
        state.pending_wipe = {"20261020_101010_abcd"};
        state.shared_wipe_pending = true;
        state.pending_seeds.push_back({"child", "parent", 42});
        return true;
    }));
    const auto state = read_agent_browser_store_state();
    EXPECT_EQ(state.cookie_mode, "per_session");
    EXPECT_EQ(state.session_stores,
              (std::vector<std::string>{"20261020_101010_abcd", "headless-run-1"}));
    EXPECT_EQ(state.pending_wipe, (std::vector<std::string>{"20261020_101010_abcd"}));
    EXPECT_TRUE(state.shared_wipe_pending);
    ASSERT_EQ(state.pending_seeds.size(), 1u);
    EXPECT_EQ(state.pending_seeds[0].session_id, "child");
    EXPECT_EQ(state.pending_seeds[0].from, "parent");
    EXPECT_EQ(state.pending_seeds[0].at_ms, 42);

    const auto file = read_file_json(path_);
    ASSERT_TRUE(file.is_object());
    const auto& section = file.at(kAgentBrowserStoreStateKey);
    EXPECT_FALSE(section.contains("pending_deletions"));
}

// 场景:state.json 里已有别的模块的字段(终端提示标记等)。
// 期望:读-改-写 agent_browser 段时其它字段原样保留。
TEST_F(AgentBrowserStoreStateTest, UpdateKeepsOtherStateKeys) {
    write_raw(path_, R"({"legacy_terminal_hint_shown":true})");
    ASSERT_TRUE(record_agent_browser_store_deletion("s1"));
    const auto file = read_file_json(path_);
    EXPECT_TRUE(file.value("legacy_terminal_hint_shown", false));
    EXPECT_EQ(read_agent_browser_store_state().pending_deletions,
              (std::vector<std::string>{"s1"}));
}

// 场景:状态段被手改坏 / 写入了异常数据:非法 id(带路径分隔符、空串、超长)、
// 重复条目、非字符串条目、子会话等于父会话的分叉记录。
// 期望:非法与重复条目被丢掉,其余按首次出现顺序保留;不抛异常。
// 非法 id 必须挡在状态文件外 —— 它会被拼进 Profile 名 / 目录名,带 `/` 的值可能越出用户数据目录。
TEST_F(AgentBrowserStoreStateTest, FromJsonDropsInvalidAndDuplicateEntries) {
    const auto section = nlohmann::json::parse(R"({
        "cookie_mode": "shared",
        "session_stores": ["a", "../escape", "", "a", 7, "b"],
        "pending_deletions": ["dir\\sub", "c"],
        "pending_seeds": [
            {"session_id": "x", "from": "x"},
            {"session_id": "y", "from": "p1", "at_ms": 1},
            {"session_id": "y", "from": "p2", "at_ms": 2},
            {"session_id": "z"},
            "garbage"
        ]
    })");
    const auto state = agent_browser_store_state_from_json(section);
    EXPECT_EQ(state.session_stores, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(state.pending_deletions, (std::vector<std::string>{"c"}));
    // 同一子会话的分叉记录以最后一条为准。
    ASSERT_EQ(state.pending_seeds.size(), 1u);
    EXPECT_EQ(state.pending_seeds[0].session_id, "y");
    EXPECT_EQ(state.pending_seeds[0].from, "p2");

    EXPECT_FALSE(valid_agent_browser_store_session_id(std::string(129, 'a')));
    EXPECT_TRUE(valid_agent_browser_store_session_id(std::string(128, 'a')));
    EXPECT_TRUE(valid_agent_browser_store_session_id("ses.~:@+_-9"));
}

// 场景:agent_browser 段不是对象(被别的工具写成字符串)。
// 期望:按空状态处理,下一次写入把它修成正常对象。
TEST_F(AgentBrowserStoreStateTest, CorruptedSectionIsTreatedAsEmpty) {
    write_raw(path_, R"({"agent_browser":"garbage"})");
    EXPECT_TRUE(read_agent_browser_store_state().session_stores.empty());
    ASSERT_TRUE(record_agent_browser_store_seed("child", "parent", 5));
    EXPECT_EQ(read_agent_browser_store_state().pending_seeds.size(), 1u);
}

// 场景:长期处在 shared 档,用户分叉了几百个会话,分叉记录永远不会被消费。
// 期望:只保留最新的 500 条,最旧的被丢掉 —— 这是防止 state.json 无限增长的上限。
TEST_F(AgentBrowserStoreStateTest, PendingSeedsAreCappedAtFiveHundredNewest) {
    ASSERT_TRUE(update_agent_browser_store_state([](AgentBrowserStoreState& state) {
        for (int i = 0; i < 505; ++i) {
            state.pending_seeds.push_back(
                {"child" + std::to_string(i), "parent", i});
        }
        return true;
    }));
    const auto state = read_agent_browser_store_state();
    ASSERT_EQ(state.pending_seeds.size(), kAgentBrowserMaxPendingSeeds);
    EXPECT_EQ(state.pending_seeds.front().session_id, "child5");
    EXPECT_EQ(state.pending_seeds.back().session_id, "child504");
}

// 场景:daemon 永久删除一个分叉出来、还没打开过浏览器的会话。
// 期望:进入待删除清单,同时撤掉它作为子会话的分叉记录(否则那条记录会一直占着名额);
// 以它为父的分叉记录保留。非法 id 不写入。
TEST_F(AgentBrowserStoreStateTest, RecordDeletionDropsSeedsOfDeletedChild) {
    ASSERT_TRUE(record_agent_browser_store_seed("forked", "main", 1));
    ASSERT_TRUE(record_agent_browser_store_seed("grandchild", "forked", 2));
    ASSERT_TRUE(record_agent_browser_store_deletion("forked"));
    EXPECT_FALSE(record_agent_browser_store_deletion("bad/id"));

    const auto state = read_agent_browser_store_state();
    EXPECT_EQ(state.pending_deletions, (std::vector<std::string>{"forked"}));
    ASSERT_EQ(state.pending_seeds.size(), 1u);
    EXPECT_EQ(state.pending_seeds[0].session_id, "grandchild");
}

// 场景:把会话分叉到它自己(不该发生的调用)/ 父或子 id 非法。
// 期望:拒绝写入,状态文件不出现这条记录。
TEST_F(AgentBrowserStoreStateTest, RecordSeedRejectsSelfAndInvalidIds) {
    EXPECT_FALSE(record_agent_browser_store_seed("same", "same", 1));
    EXPECT_FALSE(record_agent_browser_store_seed("child", "", 1));
    EXPECT_FALSE(record_agent_browser_store_seed("a b", "parent", 1));
    EXPECT_TRUE(read_agent_browser_store_state().pending_seeds.empty());
}

// 场景:所有清单都被消费完、档位也没选过。
// 期望:写回时整个 agent_browser 段被移除,不在 state.json 里留空对象。
TEST_F(AgentBrowserStoreStateTest, EmptiedSectionIsRemovedFromStateFile) {
    ASSERT_TRUE(record_agent_browser_store_deletion("s1"));
    ASSERT_TRUE(update_agent_browser_store_state([](AgentBrowserStoreState& state) {
        state.pending_deletions.clear();
        return true;
    }));
    const auto file = read_file_json(path_);
    ASSERT_TRUE(file.is_object());
    EXPECT_FALSE(file.contains(kAgentBrowserStoreStateKey));
}

// 场景:未来版本写入了本版本不认识的档位名。
// 期望:原样保留(由使用方按 shared 处理,直到用户重新选择);超长的值丢弃。
TEST_F(AgentBrowserStoreStateTest, UnknownCookieModeIsKeptVerbatimButBounded) {
    auto state = agent_browser_store_state_from_json(
        nlohmann::json{{"cookie_mode", "per_workspace"}});
    EXPECT_EQ(state.cookie_mode, "per_workspace");
    state = agent_browser_store_state_from_json(
        nlohmann::json{{"cookie_mode", std::string(64, 'x')}});
    EXPECT_TRUE(state.cookie_mode.empty());
}
