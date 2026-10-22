#include <gtest/gtest.h>

#include "web/handlers/session_batch_handler.hpp"

#include <string>
#include <vector>

using acecode::web::PinnedSessionOrderItem;
using acecode::web::SessionBatchAction;
using acecode::web::SessionBatchRequest;
using acecode::web::apply_batch_pin_order_items;
using acecode::web::apply_batch_pin_session_ids;
using acecode::web::kMaxSessionBatchItems;
using acecode::web::parse_session_batch_request;
using nlohmann::json;

namespace {

std::vector<std::string> order_keys(const std::vector<PinnedSessionOrderItem>& items) {
    std::vector<std::string> out;
    for (const auto& item : items) out.push_back(item.workspace_hash + "/" + item.session_id);
    return out;
}

} // namespace

// 场景:侧栏多选后发一次批量请求,请求体带 action 与按可见顺序排列的会话。
// 期望:动作名映射正确;无工作区任务两种写法(no_workspace 或置顶作用域名)都归一成
// no_workspace;重复会话只保留第一次出现;read 的游标原样保留。
TEST(SessionBatchHandler, ParsesItemsInOrderAndNormalizesNoWorkspaceScope) {
    SessionBatchRequest request;
    std::string error;
    ASSERT_TRUE(parse_session_batch_request(json{
        {"action", "read"},
        {"items", json::array({
            json{{"session_id", "a"}, {"workspace_hash", "ws1"}, {"cursor", 42}},
            json{{"session_id", "b"}, {"no_workspace", true}, {"workspace_hash", "ignored"}},
            json{{"session_id", "c"}, {"workspace_hash", "__no_workspace__"}},
            json{{"session_id", "a"}, {"workspace_hash", "ws1"}},
        })},
    }, request, error)) << error;

    EXPECT_EQ(request.action, SessionBatchAction::Read);
    ASSERT_EQ(request.items.size(), 3u);
    EXPECT_EQ(request.items[0].session_id, "a");
    EXPECT_EQ(request.items[0].pin_scope(), "ws1");
    EXPECT_EQ(request.items[0].cursor, 42u);
    EXPECT_TRUE(request.items[1].no_workspace);
    EXPECT_EQ(request.items[1].workspace_hash, "");
    EXPECT_EQ(request.items[1].pin_scope(), "__no_workspace__");
    EXPECT_TRUE(request.items[2].no_workspace);
    EXPECT_EQ(request.items[2].pin_scope(), "__no_workspace__");
}

// 场景:同一个会话 id 出现在两个不同工作区(各自的作用域)。
// 期望:不算重复,两项都保留 —— 去重键是「作用域 + id」而不是只看 id。
TEST(SessionBatchHandler, SameIdInDifferentScopesIsNotADuplicate) {
    SessionBatchRequest request;
    std::string error;
    ASSERT_TRUE(parse_session_batch_request(json{
        {"action", "pin"},
        {"items", json::array({
            json{{"session_id", "same"}, {"workspace_hash", "ws1"}},
            json{{"session_id", "same"}, {"workspace_hash", "ws2"}},
        })},
    }, request, error)) << error;
    EXPECT_EQ(request.items.size(), 2u);
}

// 场景:客户端发来未知动作、空列表、非法 id(含路径分隔符)或超过上限的条目数。
// 期望:全部解析失败并给出错误文案,路由据此回 400,不碰任何磁盘状态。
TEST(SessionBatchHandler, RejectsInvalidRequests) {
    SessionBatchRequest request;
    std::string error;
    EXPECT_FALSE(parse_session_batch_request(json{{"action", "delete"}, {"items", json::array({json{{"session_id", "a"}}})}}, request, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(parse_session_batch_request(json{{"action", "pin"}, {"items", json::array()}}, request, error));
    EXPECT_FALSE(parse_session_batch_request(json{{"action", "pin"}, {"items", json::array({json{{"session_id", "../x"}}})}}, request, error));
    EXPECT_FALSE(parse_session_batch_request(json{{"action", "pin"}, {"items", json::array({json{{"session_id", ""}}})}}, request, error));
    EXPECT_FALSE(parse_session_batch_request(json::array(), request, error));

    json too_many = json::array();
    for (std::size_t i = 0; i <= kMaxSessionBatchItems; ++i) {
        too_many.push_back(json{{"session_id", "s" + std::to_string(i)}, {"workspace_hash", "ws"}});
    }
    EXPECT_FALSE(parse_session_batch_request(json{{"action", "pin"}, {"items", too_many}}, request, error));
}

// 场景:作用域里已置顶 [x, y],多选 [a, x, b] 后批量置顶。
// 期望:新置顶的 a、b 按选择顺序插到最前(a 在最上面),已置顶的 x 不被挪动,
// 结果 [a, b, x, y] —— 与逐个点置顶时「已置顶的保持原位」一致。
TEST(SessionBatchHandler, BatchPinPrependsNewIdsInRequestOrderAndKeepsExistingInPlace) {
    EXPECT_EQ(apply_batch_pin_session_ids({"x", "y"}, {"a", "x", "b", "a"}, true),
              (std::vector<std::string>{"a", "b", "x", "y"}));
}

// 场景:作用域里已置顶 [a, b, c],批量取消置顶 [c, a, 不存在的 z]。
// 期望:只移除命中的 a、c,剩余顺序不变。
TEST(SessionBatchHandler, BatchUnpinRemovesOnlySelectedIds) {
    EXPECT_EQ(apply_batch_pin_session_ids({"a", "b", "c"}, {"c", "a", "z"}, false),
              (std::vector<std::string>{"b"}));
}

// 场景:全局置顶视觉顺序跨工作区,多选里有一个已经在顺序里的项。
// 期望:置顶时只把不在顺序里的项按请求顺序插到最前;取消置顶时按「作用域 + id」
// 精确移除,不会误删另一个工作区里同 id 的项。
TEST(SessionBatchHandler, BatchOrderUsesScopeAndIdAsIdentity) {
    const std::vector<PinnedSessionOrderItem> current{{"ws2", "s"}, {"ws1", "x"}};
    EXPECT_EQ(order_keys(apply_batch_pin_order_items(current, {{"ws1", "a"}, {"ws1", "x"}, {"ws3", "b"}}, true)),
              (std::vector<std::string>{"ws1/a", "ws3/b", "ws2/s", "ws1/x"}));
    EXPECT_EQ(order_keys(apply_batch_pin_order_items(current, {{"ws1", "s"}, {"ws1", "x"}}, false)),
              (std::vector<std::string>{"ws2/s"}));
}
