// 覆盖 src/domain/session/session_tree_purge.{hpp,cpp}:主会话连同全部子会话一起
// 永久删除的公共实现(Web 永久删除、TUI 设置中心、线程工具共用)。
//
// 子会话(后台任务、网状 agent)没有独立的生命周期:不能单独归档 / 删除,只随
// 主会话一起删(用户决策:事后分析要完整上下文)。一旦回归:
//   - 删除顺序漏掉子会话 → 主会话没了,子会话变成永远清不掉的孤儿记录
//   - 主会话先删 → 中途失败时设置页那一行已经消失,剩下的子会话无从重试
//   - 连带删到别的主会话的子会话 → 别的会话丢上下文

#include <gtest/gtest.h>

#include "llm/llm_provider.hpp"
#include "session/session_storage.hpp"
#include "session/session_tree_purge.hpp"
#include "session/session_user_message_search.hpp"

#include <algorithm>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using acecode::SessionMeta;
using acecode::SessionStorage;

namespace {

fs::path make_project_dir(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
               ("acecode_session_tree_purge_" + hint + "_" +
                std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// 手造一条落盘会话:meta + 一条用户消息(正文用来验证搜索索引被清掉)。
void write_session(const std::string& project_dir,
                   const std::string& id,
                   const std::string& parent,
                   const std::string& user_text) {
    SessionMeta meta;
    meta.id = id;
    meta.cwd = "/tmp/x";
    meta.created_at = "2026-10-03T00:00:00Z";
    meta.updated_at = meta.created_at;
    meta.parent_session_id = parent;
    ASSERT_TRUE(SessionStorage::write_meta(SessionStorage::meta_path(project_dir, id), meta));
    acecode::ChatMessage message;
    message.role = "user";
    message.content = user_text;
    SessionStorage::write_messages(SessionStorage::session_path(project_dir, id), {message});
}

bool session_exists(const std::string& project_dir, const std::string& id) {
    return fs::exists(SessionStorage::meta_path(project_dir, id)) ||
           fs::exists(SessionStorage::session_path(project_dir, id));
}

std::size_t index_of(const std::vector<std::string>& order, const std::string& id) {
    return static_cast<std::size_t>(std::find(order.begin(), order.end(), id) - order.begin());
}

}  // namespace

// 场景: 主会话 root 有两个子会话 a、b,a 下还挂着 a1(层级更深的子会话);另有一个
// 无关的主会话 other 带着自己的子会话。
// 期望: root 的删除顺序恰好包含 root 树里的 4 个会话,每个子会话排在它的上级之前,
// root 排最后;不含 other 树里的任何会话。没有子会话的会话只删它自己。
TEST(SessionTreePurge, DeleteOrderPutsDescendantsBeforeTheirMainSession) {
    const auto dir = make_project_dir("order");
    const std::string project_dir = dir.string();
    write_session(project_dir, "20261003-000001-root", "", "root");
    write_session(project_dir, "20261003-000002-kida", "20261003-000001-root", "a");
    write_session(project_dir, "20261003-000003-kidb", "20261003-000001-root", "b");
    write_session(project_dir, "20261003-000004-deep", "20261003-000002-kida", "a1");
    write_session(project_dir, "20261003-000005-othr", "", "other");
    write_session(project_dir, "20261003-000006-okid", "20261003-000005-othr", "other kid");

    const auto order = acecode::session_tree_delete_order(project_dir, "20261003-000001-root");
    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order.back(), "20261003-000001-root");
    EXPECT_LT(index_of(order, "20261003-000004-deep"), index_of(order, "20261003-000002-kida"))
        << "更深的子会话要排在它的上级之前";
    EXPECT_LT(index_of(order, "20261003-000003-kidb"), order.size());
    EXPECT_EQ(index_of(order, "20261003-000005-othr"), order.size());
    EXPECT_EQ(index_of(order, "20261003-000006-okid"), order.size());

    EXPECT_EQ(acecode::session_tree_delete_order(project_dir, "20261003-000006-okid"),
              std::vector<std::string>{"20261003-000006-okid"});
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// 场景: 永久删除主会话 root(带一个子会话),同目录还有一个无关的主会话。
// 期望: root 与它的子会话的 jsonl / meta 全部删除,搜索索引里也查不到它们的用户
// 消息;无关会话的数据与索引原样保留。
// 回归: TUI 设置中心曾只删主会话本身(子会话成孤儿),也没清搜索索引。
TEST(SessionTreePurge, PurgesMainSessionWithItsSubagentsAndSearchIndex) {
    const auto dir = make_project_dir("purge");
    const std::string project_dir = dir.string();
    write_session(project_dir, "20261003-000001-root", "", "root-needle");
    write_session(project_dir, "20261003-000002-kida", "20261003-000001-root", "child-needle");
    write_session(project_dir, "20261003-000005-othr", "", "other-needle");
    {
        acecode::SessionUserMessageIndex index(project_dir);
        std::string error;
        for (const char* id : {"20261003-000001-root", "20261003-000002-kida",
                               "20261003-000005-othr"}) {
            ASSERT_TRUE(index.rebuild_session(
                id, SessionStorage::session_path(project_dir, id), &error)) << error;
        }
    }

    std::string error;
    ASSERT_TRUE(acecode::purge_session_tree(project_dir, "20261003-000001-root", &error))
        << error;
    EXPECT_FALSE(session_exists(project_dir, "20261003-000001-root"));
    EXPECT_FALSE(session_exists(project_dir, "20261003-000002-kida"))
        << "子会话要随主会话一起删除";
    EXPECT_TRUE(session_exists(project_dir, "20261003-000005-othr"));

    {
        acecode::SessionUserMessageIndex index(project_dir);
        EXPECT_TRUE(index.search("root-needle", 10, &error).empty()) << error;
        EXPECT_TRUE(index.search("child-needle", 10, &error).empty()) << error;
        EXPECT_EQ(index.search("other-needle", 10, &error).size(), 1u) << error;
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
