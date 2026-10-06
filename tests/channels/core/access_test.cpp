#include <gtest/gtest.h>

#include "channels/core/access.hpp"

#include <chrono>

// channels/core/access:默认拒绝、待批准请求、机主规则(一次性 start 码 / 扫码后窗口)、
// 群与群成员分别审批。时间由调用方传入,用固定起点推进。

namespace acecode::channels::core {
namespace {

using Clock = AccessControl::Clock;
using std::chrono::minutes;
using std::chrono::seconds;

im::Inbound private_message(const std::string& user, const std::string& platform = "telegram") {
    im::Inbound inbound;
    inbound.address.platform = platform;
    inbound.address.account = "100";
    inbound.address.kind = im::ChatKind::Private;
    inbound.address.chat = user;
    inbound.address.sender = user;
    inbound.message_id = "m";
    inbound.text = "你好";
    inbound.mentioned = true;
    inbound.sender_name = "名字" + user;
    return inbound;
}

im::Inbound group_message(const std::string& group, const std::string& member, bool mentioned,
                          const std::string& platform = "telegram") {
    auto inbound = private_message(member, platform);
    inbound.address.kind = im::ChatKind::Group;
    inbound.address.chat = group;
    inbound.mentioned = mentioned;
    return inbound;
}

// 场景:陌生人第一次私聊,随后又连发一条。
// 期望:第一条得到配对提示并产生一条待批准请求(带展示名);
// 同一个人在请求有效期内再发,不再提示、不产生重复请求。
TEST(ChannelAccess, StrangerGetsOnePendingRequest) {
    AccessControl access;
    PlatformConfig config;
    const auto t0 = Clock::now();
    const auto first = access.evaluate(config, private_message("7"), t0);
    EXPECT_EQ(first.decision, AccessDecision::NotifyPending);
    ASSERT_TRUE(first.created.has_value());
    EXPECT_EQ(first.created->principal, "user:7");
    EXPECT_EQ(first.created->kind, "user");
    EXPECT_EQ(first.created->name, "名字7");
    const auto second = access.evaluate(config, private_message("7"), t0 + seconds(5));
    EXPECT_EQ(second.decision, AccessDecision::Ignore);
    EXPECT_FALSE(second.created.has_value());
    EXPECT_EQ(access.pending(t0 + seconds(5)).size(), 1u);
}

// 场景:待批准请求放置 10 分钟以上没人处理。
// 期望:9 分钟时还在;超过 10 分钟后从列表消失,审批该 id 失败;对方再发消息会产生新请求。
TEST(ChannelAccess, PendingRequestsExpireAfterTenMinutes) {
    AccessControl access;
    PlatformConfig config;
    const auto t0 = Clock::now();
    const auto created = access.evaluate(config, private_message("7"), t0).created;
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(access.pending(t0 + minutes(9)).size(), 1u);
    EXPECT_TRUE(access.pending(t0 + minutes(10) + seconds(1)).empty());
    EXPECT_FALSE(access.take(created->id, t0 + minutes(11)).has_value());
    const auto again = access.evaluate(config, private_message("7"), t0 + minutes(11));
    EXPECT_EQ(again.decision, AccessDecision::NotifyPending);
    EXPECT_TRUE(again.created.has_value());
}

// 场景:已在授权名单里的联系人私聊;其中一位是机主。
// 期望:放行;只有机主那一位的 owner 标记为 true。
TEST(ChannelAccess, ApprovedContactsAreAllowed) {
    AccessControl access;
    PlatformConfig config;
    config.owner = "user:1";
    config.access = {{"user:1", "", 0}, {"user:2", "", 0}};
    const auto now = Clock::now();
    const auto owner = access.evaluate(config, private_message("1"), now);
    EXPECT_EQ(owner.decision, AccessDecision::Allow);
    EXPECT_TRUE(owner.owner);
    const auto contact = access.evaluate(config, private_message("2"), now);
    EXPECT_EQ(contact.decision, AccessDecision::Allow);
    EXPECT_FALSE(contact.owner);
}

// 场景:Telegram 机主绑定链接里的一次性码:正确使用一次、被第二个人重复使用、过期后使用。
// 期望:第一次使用的人成为机主(claimed_owner);码用过即作废,第二个人只得到配对提示;
// 过期的码同样无效。
TEST(ChannelAccess, OwnerStartCodeIsSingleUseAndExpires) {
    AccessControl access;
    PlatformConfig config;
    const auto t0 = Clock::now();
    const auto code = access.issue_owner_code(t0);
    ASSERT_EQ(code.size(), 24u);
    auto claim = private_message("1");
    claim.start_code = code;
    const auto first = access.evaluate(config, claim, t0 + minutes(1));
    EXPECT_EQ(first.decision, AccessDecision::Allow);
    EXPECT_TRUE(first.claimed_owner);
    EXPECT_TRUE(first.owner);

    auto reuse = private_message("2");
    reuse.start_code = code;
    const auto second = access.evaluate(config, reuse, t0 + minutes(2));
    EXPECT_FALSE(second.claimed_owner);
    EXPECT_EQ(second.decision, AccessDecision::NotifyPending);

    const auto late_code = access.issue_owner_code(t0);
    auto late = private_message("3");
    late.start_code = late_code;
    const auto expired = access.evaluate(config, late, t0 + minutes(10) + seconds(1));
    EXPECT_FALSE(expired.claimed_owner);
}

// 场景:QQ 扫码成功但绑定服务没给扫码人身份,打开 10 分钟的机主窗口。
// 期望:窗口内第一个私聊的人成为机主,窗口随即关闭;第二个人走配对审批。
TEST(ChannelAccess, OwnerWindowClaimsFirstPrivateSender) {
    AccessControl access;
    PlatformConfig config;
    const auto t0 = Clock::now();
    access.open_owner_window(t0);
    EXPECT_TRUE(access.owner_window_open(t0 + minutes(1)));
    const auto first = access.evaluate(config, private_message("OPENID1", "qq"), t0 + minutes(1));
    EXPECT_TRUE(first.claimed_owner);
    EXPECT_EQ(first.principal, "user:OPENID1");
    EXPECT_FALSE(access.owner_window_open(t0 + minutes(1)));
    const auto second = access.evaluate(config, private_message("OPENID2", "qq"), t0 + minutes(2));
    EXPECT_FALSE(second.claimed_owner);
    EXPECT_EQ(second.decision, AccessDecision::NotifyPending);
}

// 场景:平台已经有机主,此时窗口或绑定码仍在有效期内,陌生人私聊。
// 期望:不会抢走机主身份,只产生待批准请求。
TEST(ChannelAccess, ExistingOwnerIsNeverReplacedImplicitly) {
    AccessControl access;
    PlatformConfig config;
    config.owner = "user:1";
    config.access = {{"user:1", "", 0}};
    const auto t0 = Clock::now();
    access.open_owner_window(t0);
    auto stranger = private_message("9");
    stranger.start_code = access.issue_owner_code(t0);
    const auto result = access.evaluate(config, stranger, t0 + seconds(1));
    EXPECT_FALSE(result.claimed_owner);
    EXPECT_FALSE(result.owner);
    EXPECT_EQ(result.decision, AccessDecision::NotifyPending);
}

// 场景:群聊的完整审批顺序:未 @ 的消息、未批准的群里被 @、群批准后未授权成员被 @、
// 成员被授权后再 @。
// 期望:未 @ 直接忽略且不产生请求;未批准的群只在设置页产生群请求、不在群里回复;
// 群批准后,未授权成员得到一次提示并产生成员请求;授权后放行。
// 每一步的判定原因都写进日志(设置页不再展示),排查“群里为什么不回”时靠它。
TEST(ChannelAccess, GroupNeedsApprovalThenMemberApprovalAndMention) {
    AccessControl access;
    PlatformConfig config;
    const auto now = Clock::now();
    const auto silent = access.evaluate(config, group_message("-100", "5", false), now);
    EXPECT_EQ(silent.decision, AccessDecision::Ignore);
    EXPECT_FALSE(silent.created.has_value());
    EXPECT_EQ(silent.reason, "group message without @mention, ignored");

    const auto unapproved = access.evaluate(config, group_message("-100", "5", true), now);
    EXPECT_EQ(unapproved.decision, AccessDecision::Ignore);
    ASSERT_TRUE(unapproved.created.has_value());
    EXPECT_EQ(unapproved.created->principal, "group:-100");
    EXPECT_EQ(unapproved.created->kind, "group");
    EXPECT_EQ(unapproved.reason, "group not approved, approval requested");
    EXPECT_EQ(access.evaluate(config, group_message("-100", "5", true), now).reason,
              "group not approved, a request is already pending");

    config.access.push_back({"group:-100", "", 0});
    const auto member = access.evaluate(config, group_message("-100", "5", true), now);
    EXPECT_EQ(member.decision, AccessDecision::NotifyPending);
    ASSERT_TRUE(member.created.has_value());
    EXPECT_EQ(member.created->principal, "user:5");
    EXPECT_EQ(member.reason, "group member not authorized, approval requested");

    config.access.push_back({"user:5", "", 0});
    const auto allowed = access.evaluate(config, group_message("-100", "5", true), now);
    EXPECT_EQ(allowed.decision, AccessDecision::Allow);
    EXPECT_EQ(allowed.reason, "authorized");
    EXPECT_EQ(access.evaluate(config, group_message("-100", "5", false), now).decision, AccessDecision::Ignore);
}

// 场景:QQ 群成员的 openid 按群隔离,与私聊 openid 不同;Telegram 用户 id 全局唯一。
// 期望:QQ 群成员身份是 member:<群>:<成员>,Telegram 群成员仍是 user:<id>。
TEST(ChannelAccess, QqGroupMembersArePerGroupPrincipals) {
    EXPECT_EQ(principal_for(group_message("G1", "M1", true, "qq").address), "member:G1:M1");
    EXPECT_EQ(principal_for(group_message("G2", "M1", true, "qq").address), "member:G2:M1");
    EXPECT_EQ(principal_for(group_message("-100", "5", true, "telegram").address), "user:5");
    EXPECT_EQ(principal_kind("member:G1:M1"), "member");
    EXPECT_EQ(principal_kind("group:G1"), "group");
    EXPECT_EQ(principal_kind("user:5"), "user");
}

// 场景:机主撤销某人的授权时,对方还有一条待批准请求。
// 期望:drop_principal 把该身份的请求一并移除,其他人的请求保留。
TEST(ChannelAccess, DropPrincipalRemovesItsRequests) {
    AccessControl access;
    PlatformConfig config;
    const auto now = Clock::now();
    access.evaluate(config, private_message("7"), now);
    access.evaluate(config, private_message("8"), now);
    access.drop_principal("user:7");
    const auto pending = access.pending(now);
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].principal, "user:8");
}

} // namespace
} // namespace acecode::channels::core
