#include <gtest/gtest.h>

#include "utils/token_tracker.hpp"

TEST(TokenTracker, ContextPercentRoundsAndClamps) {
    EXPECT_EQ(acecode::TokenTracker::context_percent_for(0, 128000), 0);
    EXPECT_EQ(acecode::TokenTracker::context_percent_for(10240, 128000), 8);
    EXPECT_EQ(acecode::TokenTracker::context_percent_for(51200, 128000), 40);
    EXPECT_EQ(acecode::TokenTracker::context_percent_for(128001, 128000), 100);
    EXPECT_EQ(acecode::TokenTracker::context_percent_for(1000, 0), 0);
}

TEST(TokenTracker, ContextPercentUsesLastPromptTokens) {
    acecode::TokenTracker tracker;
    acecode::TokenUsage usage;
    usage.prompt_tokens = 76800;
    usage.total_tokens = 80000;
    usage.has_data = true;

    tracker.record(usage);

    EXPECT_EQ(tracker.context_percent(128000), 60);
}

TEST(TokenTracker, CacheHitPercentRoundsAndClamps) {
    EXPECT_EQ(acecode::TokenTracker::cache_hit_percent_for(1000, 870), 87);
    EXPECT_EQ(acecode::TokenTracker::cache_hit_percent_for(1000, 0), 0);
    EXPECT_EQ(acecode::TokenTracker::cache_hit_percent_for(1000, 1000), 100);
    // A cached count larger than the total input would mean the provider
    // reported overlapping counters; clamp rather than show >100%.
    EXPECT_EQ(acecode::TokenTracker::cache_hit_percent_for(1000, 1200), 100);
    // Unknown, not zero: nothing to divide by yet.
    EXPECT_EQ(acecode::TokenTracker::cache_hit_percent_for(0, 0), -1);
}

TEST(TokenTracker, CacheHitPercentIsUnknownUntilServerReportsUsage) {
    acecode::TokenTracker tracker;
    EXPECT_EQ(tracker.cache_hit_percent(), -1);
    EXPECT_EQ(tracker.last_cache_hit_percent(), -1);
    EXPECT_TRUE(tracker.format_cache_status().empty());
}

TEST(TokenTracker, CacheHitPercentAccumulatesOverSession) {
    acecode::TokenTracker tracker;

    acecode::TokenUsage cold;
    cold.prompt_tokens = 1000;
    cold.cache_read_tokens = 0;
    cold.has_data = true;
    tracker.record(cold);
    EXPECT_EQ(tracker.cache_hit_percent(), 0);
    EXPECT_EQ(tracker.format_cache_status(), "cache 0%");

    acecode::TokenUsage warm;
    warm.prompt_tokens = 1000;
    warm.cache_read_tokens = 900;
    warm.has_data = true;
    tracker.record(warm);

    // Session: 900 cached of 2000 total input. Last call alone: 90%.
    EXPECT_EQ(tracker.cache_hit_percent(), 45);
    EXPECT_EQ(tracker.last_cache_hit_percent(), 90);
    EXPECT_EQ(tracker.format_cache_status(), "cache 45%");
}

// A provider that reports usage but never reports cache counters reads as 0%,
// which is the honest answer and keeps a missing-cache regression visible.
TEST(TokenTracker, ProviderWithoutCacheCountersReadsAsZeroNotUnknown) {
    acecode::TokenTracker tracker;
    acecode::TokenUsage usage;
    usage.prompt_tokens = 4096;
    usage.has_data = true;
    tracker.record(usage);

    EXPECT_EQ(tracker.cache_hit_percent(), 0);
    EXPECT_EQ(tracker.format_cache_status(), "cache 0%");
}
