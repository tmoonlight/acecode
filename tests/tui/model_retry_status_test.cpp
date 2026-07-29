#include "tui/model_retry_status.hpp"

#include <gtest/gtest.h>

using acecode::tui::model_retry_resume_phrase;
using acecode::tui::model_retry_wait_phrase;

TEST(ModelRetryStatus, FormatsEnglishSecondsAndRoundedMinutes) {
    EXPECT_EQ(model_retry_wait_phrase(false, 1),
              "Network unavailable, retrying in 1 sec");
    EXPECT_EQ(model_retry_wait_phrase(false, 60'001),
              "Network unavailable, retrying in 2 min");
    EXPECT_EQ(model_retry_wait_phrase(false, 20 * 60 * 1000),
              "Network unavailable, retrying in 20 min");
}

TEST(ModelRetryStatus, FormatsChineseAndClampsNegativeDelay) {
    EXPECT_EQ(model_retry_wait_phrase(true, -1),
              "网络暂时不可用，0 秒后重试");
    EXPECT_EQ(model_retry_wait_phrase(true, 60'000),
              "网络暂时不可用，1 分钟后重试");
}

TEST(ModelRetryStatus, FormatsResumeStateForBothLocales) {
    EXPECT_EQ(model_retry_resume_phrase(false), "Reconnecting");
    EXPECT_EQ(model_retry_resume_phrase(true), "正在重新连接");
}
