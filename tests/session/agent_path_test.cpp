// 覆盖蜂群模式（网状）的 agent 路径纯逻辑(src/domain/session/agent_path.*),
// 规则逐条对照 Codex codex-rs/protocol/src/agent_path.rs:
//   1. 根路径 /root 的名字、深度、父路径
//   2. join 拼出子路径,任务名非法时返回 Codex 原文错误
//   3. resolve 区分 canonical(/root 开头)与相对引用
//   4. 兄弟子树之间只能用 canonical 路径互相寻址
//   5. 绝对路径校验:不以 /root 开头、以 / 结尾、段名非法都拒绝
//   6. has_prefix 按段匹配,不把 /root/ab 当成 /root/a 的后代

#include <gtest/gtest.h>

#include "session/agent_path.hpp"

using acecode::mesh::AgentPath;
using acecode::mesh::validate_agent_name;

// 场景:取根路径的基本属性。
// 期望:字符串为 "/root",名字为 "root",深度 0,没有父路径。
TEST(MeshAgentPath, RootHasNameRootAndNoParent) {
    const AgentPath root = AgentPath::root();
    EXPECT_EQ(root.str(), "/root");
    EXPECT_TRUE(root.is_root());
    EXPECT_EQ(root.name(), "root");
    EXPECT_EQ(root.depth(), 0);
    EXPECT_FALSE(root.parent().has_value());
}

// 场景:/root 依次 join "researcher" 与 "worker"(二级嵌套)。
// 期望:得到 /root/researcher/worker,名字为最后一段,深度 2,父路径逐级回退。
TEST(MeshAgentPath, JoinBuildsNestedChildPaths) {
    std::string error;
    const auto child = AgentPath::root().join("researcher", &error);
    ASSERT_TRUE(child.has_value()) << error;
    const auto grandchild = child->join("worker", &error);
    ASSERT_TRUE(grandchild.has_value()) << error;
    EXPECT_EQ(grandchild->str(), "/root/researcher/worker");
    EXPECT_EQ(grandchild->name(), "worker");
    EXPECT_EQ(grandchild->depth(), 2);
    ASSERT_TRUE(grandchild->parent().has_value());
    EXPECT_EQ(grandchild->parent()->str(), "/root/researcher");
}

// 场景:join 的任务名含大写/连字符、为保留名 root/./..、含斜杠或为空。
// 期望:全部拒绝,错误文案与 Codex validate_agent_name 逐字一致(模型看到的
// 提示与 Codex 训练分布一致)。
TEST(MeshAgentPath, JoinRejectsInvalidTaskNamesWithCodexMessages) {
    std::string error;
    EXPECT_FALSE(AgentPath::root().join("BadName", &error).has_value());
    EXPECT_EQ(error, "agent_name must use only lowercase letters, digits, and underscores");
    EXPECT_FALSE(AgentPath::root().join("review-tests", &error).has_value());
    EXPECT_EQ(error, "agent_name must use only lowercase letters, digits, and underscores");
    EXPECT_FALSE(AgentPath::root().join("root", &error).has_value());
    EXPECT_EQ(error, "agent_name `root` is reserved");
    EXPECT_FALSE(AgentPath::root().join("..", &error).has_value());
    EXPECT_EQ(error, "agent_name `..` is reserved");
    EXPECT_FALSE(AgentPath::root().join("a/b", &error).has_value());
    EXPECT_EQ(error, "agent_name must not contain `/`");
    EXPECT_EQ(validate_agent_name(""), "agent_name must not be empty");
    EXPECT_EQ(validate_agent_name("task_3"), "");
}

// 场景:当前 agent 为 /root/task1,分别解析相对名 "task_3"、canonical
// "/root/task1/task_3"、"/root"。
// 期望:前两者都指向 /root/task1/task_3(可互换引用),"/root" 解析为根。
TEST(MeshAgentPath, ResolveTreatsRelativeAndCanonicalInterchangeably) {
    const auto current = AgentPath::parse("/root/task1");
    ASSERT_TRUE(current.has_value());
    const auto relative = current->resolve("task_3");
    const auto canonical = current->resolve("/root/task1/task_3");
    ASSERT_TRUE(relative.has_value());
    ASSERT_TRUE(canonical.has_value());
    EXPECT_EQ(relative->str(), "/root/task1/task_3");
    EXPECT_EQ(*relative, *canonical);
    const auto root = current->resolve("/root");
    ASSERT_TRUE(root.has_value());
    EXPECT_TRUE(root->is_root());
}

// 场景:兄弟子树 /root/task2/task_3 想引用 /root/task1/task_3,却用了相对名
// "task_3";以及用 "../sibling" 试图向上跳。
// 期望:相对名解析到自己名下的 /root/task2/task_3/task_3(与 Codex 一致,只有
// canonical 路径能跨子树);".." 作为段名被拒绝,不存在向上相对寻址。
TEST(MeshAgentPath, SiblingSubtreesNeedCanonicalPaths) {
    const auto current = AgentPath::parse("/root/task2/task_3");
    ASSERT_TRUE(current.has_value());
    const auto relative = current->resolve("task_3");
    ASSERT_TRUE(relative.has_value());
    EXPECT_EQ(relative->str(), "/root/task2/task_3/task_3");
    std::string error;
    EXPECT_FALSE(current->resolve("../sibling", &error).has_value());
    EXPECT_EQ(error, "agent_name `..` is reserved");
    EXPECT_FALSE(current->resolve("", &error).has_value());
    EXPECT_EQ(error, "agent path must not be empty");
    EXPECT_FALSE(current->resolve("task_3/", &error).has_value());
    EXPECT_EQ(error, "relative agent path must not end with `/`");
}

// 场景:解析不以 /root 开头、以 / 结尾、段名非法的绝对路径,以及 Codex 保留的
// /morpheus(ACECode 不支持)。
// 期望:全部拒绝并给出对应的 Codex 文案;合法的多级路径被接受。
TEST(MeshAgentPath, ParseValidatesAbsolutePaths) {
    std::string error;
    EXPECT_FALSE(AgentPath::parse("/not-root", &error).has_value());
    EXPECT_EQ(error, "absolute agent paths must start with `/root` or be `/morpheus`");
    EXPECT_FALSE(AgentPath::parse("root/a", &error).has_value());
    EXPECT_EQ(error, "absolute agent paths must start with `/root` or be `/morpheus`");
    EXPECT_FALSE(AgentPath::parse("/morpheus", &error).has_value());
    EXPECT_FALSE(AgentPath::parse("/root/a/", &error).has_value());
    EXPECT_EQ(error, "absolute agent path must not end with `/`");
    EXPECT_FALSE(AgentPath::parse("/root/A", &error).has_value());
    EXPECT_EQ(error, "agent_name must use only lowercase letters, digits, and underscores");
    EXPECT_TRUE(AgentPath::parse("/root/a_1/b2").has_value());
}

// 场景:agent_list(path_prefix) 的过滤判定,前缀 "/root/a"。
// 期望:/root/a 与 /root/a/x 命中,/root/ab(只是字符串前缀相同)不命中;
// 空前缀命中一切。
TEST(MeshAgentPath, HasPrefixMatchesWholeSegments) {
    const auto a = AgentPath::parse("/root/a");
    const auto ax = AgentPath::parse("/root/a/x");
    const auto ab = AgentPath::parse("/root/ab");
    ASSERT_TRUE(a && ax && ab);
    EXPECT_TRUE(a->has_prefix("/root/a"));
    EXPECT_TRUE(ax->has_prefix("/root/a"));
    EXPECT_FALSE(ab->has_prefix("/root/a"));
    EXPECT_TRUE(ab->has_prefix(""));
    EXPECT_TRUE(ab->has_prefix("/root"));
}
