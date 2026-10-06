#include <gtest/gtest.h>

#include "im/transport.hpp"

// im/transport:IM 会话地址是会话绑定、去重回执和授权记录的主键。

namespace acecode::im {
namespace {

Address private_chat() {
    Address a;
    a.platform = "telegram";
    a.account = "8001";
    a.kind = ChatKind::Private;
    a.chat = "42";
    a.sender = "42";
    return a;
}

// 场景:同一个人分别在私聊、群 A、群 B 里发消息;群里还有另一个成员。
// 期望:四个地址的会话键各不相同;话题 id 不参与会话键(同一人在群里不同话题仍是同一会话)。
TEST(ImAddress, KeysSeparatePrivateGroupsAndSenders) {
    const auto dm = private_chat();
    auto group_a = dm;
    group_a.kind = ChatKind::Group;
    group_a.chat = "-1001";
    auto group_b = group_a;
    group_b.chat = "-1002";
    auto other_member = group_a;
    other_member.sender = "43";
    EXPECT_NE(dm.key(), group_a.key());
    EXPECT_NE(group_a.key(), group_b.key());
    EXPECT_NE(group_a.key(), other_member.key());
    auto with_thread = group_a;
    with_thread.thread = "7";
    EXPECT_EQ(group_a.key(), with_thread.key());
}

// 场景:地址写入 state.json 后再读回。
// 期望:往返后字段与会话键完全一致;kind 非法时读取失败而不是默认成私聊。
TEST(ImAddress, JsonRoundTripAndRejectsUnknownKind) {
    auto group = private_chat();
    group.kind = ChatKind::Group;
    group.chat = "-1001";
    group.thread = "9";
    const auto restored = Address::from_json(group.to_json());
    EXPECT_EQ(restored.key(), group.key());
    EXPECT_EQ(restored.thread, "9");
    auto bad = group.to_json();
    bad["kind"] = "channel";
    EXPECT_THROW(Address::from_json(bad), std::runtime_error);
}

// 场景:校验来自平台的地址。
// 期望:私聊要求 chat 与 sender 相同;未知平台、空 id、含空格或换行的 id 一律无效;
// 钉钉 base64 形式的会话 id(含 + / =)与 $:LWCP_v1:$ 形式的发送人 id、微信 …@im.wechat 有效。
TEST(ImAddress, ValidationRejectsMalformedAddresses) {
    EXPECT_TRUE(private_chat().valid());
    auto mismatched = private_chat();
    mismatched.sender = "43";
    EXPECT_FALSE(mismatched.valid());
    auto unknown = private_chat();
    unknown.platform = "whatsapp";
    EXPECT_FALSE(unknown.valid());
    auto spaced = private_chat();
    spaced.chat = spaced.sender = "a b";
    EXPECT_FALSE(spaced.valid());
    auto newline = private_chat();
    newline.chat = newline.sender = "a\nb";
    EXPECT_FALSE(newline.valid());
    auto dingtalk = private_chat();
    dingtalk.platform = "dingtalk";
    dingtalk.kind = ChatKind::Group;
    dingtalk.chat = "cidb4uF+pXo/FpKMw==";
    dingtalk.sender = "$:LWCP_v1:$abcDEF";
    EXPECT_TRUE(dingtalk.valid());
    auto weixin = private_chat();
    weixin.platform = "weixin";
    weixin.chat = weixin.sender = "o9cq80wW@im.wechat";
    EXPECT_TRUE(weixin.valid());
    auto empty = private_chat();
    empty.account.clear();
    EXPECT_FALSE(empty.valid());
}

} // namespace
} // namespace acecode::im
