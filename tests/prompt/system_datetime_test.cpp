#include <gtest/gtest.h>

#include "prompt/system_datetime.hpp"

#include <algorithm>
#include <ctime>

TEST(SystemDatetimeTest, FormatsUtcOffsets) {
    EXPECT_EQ(acecode::format_utc_offset(0), "UTC+00:00");
    EXPECT_EQ(acecode::format_utc_offset(480), "UTC+08:00");
    EXPECT_EQ(acecode::format_utc_offset(-330), "UTC-05:30");
}

TEST(SystemDatetimeTest, FormatsPromptDatetimeWithWeekday) {
    std::tm local{};
    local.tm_year = 2026 - 1900;
    local.tm_mon = 5 - 1;
    local.tm_mday = 15;
    local.tm_hour = 19;
    local.tm_min = 32;
    local.tm_sec = 10;
    local.tm_wday = 5;

    EXPECT_EQ(
        acecode::format_prompt_datetime(local, 480),
        "2026-05-15 19:32:10 UTC+08:00 (Friday)");
}

TEST(SystemDatetimeTest, FormatsPromptDateWithoutTimeOfDay) {
    std::tm local{};
    local.tm_year = 2026 - 1900;
    local.tm_mon = 5 - 1;
    local.tm_mday = 15;
    local.tm_hour = 19;
    local.tm_min = 32;
    local.tm_sec = 10;
    local.tm_wday = 5;

    EXPECT_EQ(
        acecode::format_prompt_date(local, 480),
        "2026-05-15 (Friday, UTC+08:00)");
}

// The date-only form is what the static system prompt carries. Two calls made
// within the same day must be byte-identical, otherwise the system prompt —
// the very front of the cacheable prefix — changes on every provider request.
TEST(SystemDatetimeTest, CurrentPromptDateIsStableAcrossCalls) {
    const std::string first = acecode::current_prompt_date();
    const std::string second = acecode::current_prompt_date();
    EXPECT_EQ(first, second);

    // No time of day. The only ':' allowed is the one inside the UTC offset,
    // which lives in the trailing "(Weekday, UTC+hh:mm)" parenthetical.
    const std::size_t paren = first.find(" (");
    ASSERT_NE(paren, std::string::npos);
    EXPECT_EQ(first.substr(0, paren).find(':'), std::string::npos);
    EXPECT_EQ(std::count(first.begin(), first.end(), ':'), 1);
}

TEST(SystemDatetimeTest, CurrentPromptDatetimeContainsRequiredShape) {
    const std::string value = acecode::current_prompt_datetime();

    ASSERT_GE(value.size(), 35u);
    EXPECT_EQ(value[4], '-');
    EXPECT_EQ(value[7], '-');
    EXPECT_EQ(value[10], ' ');
    EXPECT_EQ(value[13], ':');
    EXPECT_EQ(value[16], ':');
    EXPECT_NE(value.find(" UTC"), std::string::npos);
    EXPECT_NE(value.find("("), std::string::npos);
    EXPECT_NE(value.find(")"), std::string::npos);

    for (unsigned char c : value) {
        EXPECT_LT(c, 128);
    }
}
