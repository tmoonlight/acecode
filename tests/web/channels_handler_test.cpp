#include <gtest/gtest.h>

#include "web/handlers/channels_handler.hpp"

// web/handlers/channels_handler:消息通道 REST 接口的请求校验与会话列表标注(纯函数)。

namespace acecode::web {
namespace {

using nlohmann::json;

// 场景:开关请求体分别是正确的布尔值、字符串、缺字段、不是对象。
// 期望:只有 {"enabled": bool} 被接受,其余返回中文错误。
TEST(ChannelsHandler, EnabledRequestNeedsBoolean) {
    bool enabled = false;
    std::string error;
    EXPECT_TRUE(parse_channel_enabled_request(json{{"enabled", true}}, enabled, error));
    EXPECT_TRUE(enabled);
    EXPECT_FALSE(parse_channel_enabled_request(json{{"enabled", "yes"}}, enabled, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(parse_channel_enabled_request(json::object(), enabled, error));
    EXPECT_FALSE(parse_channel_enabled_request(json::array(), enabled, error));
}

// 场景:QQ 手动填写凭据:两端带空白、只改其中一个字段、混入未知字段、全部为空。
// 期望:去空白后返回;空字符串字段省略(沿用已保存的值);未知字段与全空都被拒绝。
TEST(ChannelsHandler, QqCredentialsAreTrimmedAndRestricted) {
    std::string error;
    const auto both = parse_channel_credentials_request("qq", json{{"app_id", " 1020 "}, {"app_secret", "s3cret\n"}},
                                                        error);
    ASSERT_TRUE(both.has_value()) << error;
    EXPECT_EQ(both->value("app_id", ""), "1020");
    EXPECT_EQ(both->value("app_secret", ""), "s3cret");

    const auto only_id = parse_channel_credentials_request("qq", json{{"app_id", "1020"}, {"app_secret", ""}}, error);
    ASSERT_TRUE(only_id.has_value()) << error;
    EXPECT_FALSE(only_id->contains("app_secret"));

    EXPECT_FALSE(parse_channel_credentials_request("qq", json{{"token", "x"}}, error).has_value());
    EXPECT_FALSE(parse_channel_credentials_request("qq", json{{"app_id", "  "}}, error).has_value());
    EXPECT_FALSE(parse_channel_credentials_request("qq", json{{"app_id", 1020}}, error).has_value());
}

// 场景:Telegram token 过长,或请求体不是对象。
// 期望:拒绝并给出原因。
TEST(ChannelsHandler, TelegramTokenLengthIsLimited) {
    std::string error;
    EXPECT_TRUE(parse_channel_credentials_request("telegram", json{{"token", "100:abc"}}, error).has_value());
    EXPECT_FALSE(
        parse_channel_credentials_request("telegram", json{{"token", std::string(600, 'a')}}, error).has_value());
    EXPECT_FALSE(parse_channel_credentials_request("telegram", json("100:abc"), error).has_value());
}

// 场景:撤销授权的路径参数:百分号编码的 QQ 群成员身份、未编码的用户身份、未知前缀、坏编码。
// 期望:解码后符合 user:/group:/member: 前缀的才接受。
TEST(ChannelsHandler, PrincipalPathSegmentIsDecodedAndValidated) {
    EXPECT_EQ(decode_channel_principal("member%3AG1%3AM1").value_or(""), "member:G1:M1");
    EXPECT_EQ(decode_channel_principal("user:123").value_or(""), "user:123");
    EXPECT_EQ(decode_channel_principal("group%3A-100").value_or(""), "group:-100");
    EXPECT_FALSE(decode_channel_principal("admin").has_value());
    EXPECT_FALSE(decode_channel_principal("user%3").has_value());
    EXPECT_FALSE(decode_channel_principal("user%3A%00x").has_value());
    EXPECT_FALSE(decode_channel_principal("").has_value());
}

// 场景:会话列表三行,其中一行被 QQ 绑定。
// 期望:只有那一行加 channel_bound:{platform:"qq"},其它行不加字段。
TEST(ChannelsHandler, AnnotatesOnlyBoundSessions) {
    json rows = json::array({{{"id", "a"}}, {{"id", "b"}}, {{"title", "无 id"}}});
    annotate_channel_bound(rows, {{"b", "qq"}});
    EXPECT_FALSE(rows[0].contains("channel_bound"));
    EXPECT_EQ(rows[1]["channel_bound"].value("platform", ""), "qq");
    EXPECT_FALSE(rows[2].contains("channel_bound"));
    EXPECT_TRUE(is_channel_platform("telegram"));
    EXPECT_FALSE(is_channel_platform("whatsapp"));  // WhatsApp 不归消息通道页面管理
}

} // namespace
} // namespace acecode::web
