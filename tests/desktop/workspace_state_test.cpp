// 场景:迁移原状态持久化用例,保持文件格式、容错与失败返回语义。
#include <gtest/gtest.h>
#include "desktop/workspace_state.hpp"
#include "test_support/utils/state_file_fixture.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using acecode::test_support::StateFileTest;
using acecode::test_support::write_raw;

// 场景:last_active_workspace_hash 序列化往返 — desktop 多 workspace 模型靠这条
// 跨启动持久化"上次活跃 workspace"。
TEST_F(StateFileTest, LastActiveWorkspaceHashRoundTrip) {
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), ""); // 初始空
    acecode::write_last_active_workspace_hash("abc1234567890def");
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "abc1234567890def");
    // 覆盖写
    acecode::write_last_active_workspace_hash("ffffffffffffffff");
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "ffffffffffffffff");
    // 共存其他 key 不互相覆盖
    acecode::write_state_flag("some_flag", true);
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "ffffffffffffffff");
    EXPECT_TRUE(acecode::read_state_flag("some_flag"));
}

// 场景:last_active_workspace_hash 字段类型不对(数字)→ read 返回空字符串而不是抛
TEST_F(StateFileTest, LastActiveWrongTypeReadsEmpty) {
    write_raw(path_, R"({"last_active_workspace_hash": 12345})");
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "");
}

// 场景:首页 workspace 选择器跨 desktop 启动保存上次选择。
// 空字符串是有效选择,表示"不使用工作区"。
TEST_F(StateFileTest, LastHomeWorkspaceHashRoundTripAllowsEmpty) {
    EXPECT_EQ(acecode::read_last_home_workspace_hash(), "");
    acecode::write_last_home_workspace_hash("abc1234567890def");
    EXPECT_EQ(acecode::read_last_home_workspace_hash(), "abc1234567890def");

    acecode::write_last_home_workspace_hash("");
    EXPECT_EQ(acecode::read_last_home_workspace_hash(), "");

    acecode::write_state_flag("some_flag", true);
    EXPECT_EQ(acecode::read_last_home_workspace_hash(), "");
    EXPECT_TRUE(acecode::read_state_flag("some_flag"));
}

// 场景:last_home_workspace_hash 字段类型不对 → read 返回空字符串而不是抛。
TEST_F(StateFileTest, LastHomeWorkspaceWrongTypeReadsEmpty) {
    write_raw(path_, R"({"last_home_workspace_hash": 12345})");
    EXPECT_EQ(acecode::read_last_home_workspace_hash(), "");
}
