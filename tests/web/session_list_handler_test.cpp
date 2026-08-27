#include <gtest/gtest.h>

#include "web/handlers/session_list_handler.hpp"

#include <nlohmann/json.hpp>
#include <string>

using acecode::web::bounded_session_list_body;
using acecode::web::parse_session_list_limit;
using nlohmann::json;

TEST(SessionListHandler, ParsesLimitAndTreatsMissingOrInvalidAsUnlimited) {
    EXPECT_EQ(parse_session_list_limit(nullptr), 0);
    EXPECT_EQ(parse_session_list_limit(""), 0);
    EXPECT_EQ(parse_session_list_limit("abc"), 0);
    EXPECT_EQ(parse_session_list_limit("0"), 0);
    EXPECT_EQ(parse_session_list_limit("-3"), 0);
    EXPECT_EQ(parse_session_list_limit("5"), 5);
    EXPECT_EQ(parse_session_list_limit("10001"), 10000);
}

TEST(SessionListHandler, UnlimitedListsStayRawArrays) {
    json sessions = json::array({ {{"id", "a"}}, {{"id", "b"}} });
    const json body = bounded_session_list_body(sessions, 2, 0);
    ASSERT_TRUE(body.is_array());
    EXPECT_EQ(body.size(), 2u);
}

TEST(SessionListHandler, LimitedListsWrapSessionsAndTotal) {
    json sessions = json::array({ {{"id", "a"}}, {{"id", "b"}} });
    const json body = bounded_session_list_body(std::move(sessions), 9, 2);
    ASSERT_TRUE(body.is_object());
    EXPECT_EQ(body.at("total").get<std::size_t>(), 9u);
    ASSERT_TRUE(body.at("sessions").is_array());
    EXPECT_EQ(body.at("sessions").size(), 2u);
    EXPECT_EQ(body.at("sessions")[0].at("id"), "a");
    // 默认参数保持旧调用点的语义:读完了、没有更多。
    EXPECT_TRUE(body.at("total_exact").get<bool>());
    EXPECT_FALSE(body.at("has_more").get<bool>());
}

// 触发场景:分页在读满一页后就不再扫描项目目录,精确的过滤后总数无从得知,
// 只能按目录里的候选文件数报一个偏大的上界。
// 期望行为:信封如实标出 total_exact=false 并给出 has_more —— 客户端要判断
// 「还有没加载的会话」只能看 has_more,拿 sessions.size() 与这个上界相比会
// 得出错误结论。
TEST(SessionListHandler, TruncatedPagesMarkTotalInexactAndFlagMore) {
    json sessions = json::array({ {{"id", "a"}} });
    const json body = bounded_session_list_body(
        std::move(sessions), 1428, 1, /*total_exact=*/false, /*has_more=*/true);
    ASSERT_TRUE(body.is_object());
    EXPECT_EQ(body.at("total").get<std::size_t>(), 1428u);
    EXPECT_FALSE(body.at("total_exact").get<bool>());
    EXPECT_TRUE(body.at("has_more").get<bool>());
}

// 触发场景:无 limit 的老调用方(以及 /api/sessions)仍要拿到裸数组。
// 期望行为:limit<=0 时分页字段一个都不加,响应体形状与改动前逐字节一致。
TEST(SessionListHandler, UnlimitedListsNeverGainPagingFields) {
    json sessions = json::array({ {{"id", "a"}} });
    const json body = bounded_session_list_body(
        std::move(sessions), 1, 0, /*total_exact=*/false, /*has_more=*/true);
    ASSERT_TRUE(body.is_array());
    EXPECT_EQ(body.size(), 1u);
}
