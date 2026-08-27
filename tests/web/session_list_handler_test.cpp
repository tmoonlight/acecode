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
}
