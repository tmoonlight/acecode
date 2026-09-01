#include "tui/message_render_cache.hpp"
#include <gtest/gtest.h>
#include <ftxui/dom/elements.hpp>

namespace acecode::tui {
using namespace ftxui;

TEST(MessageRenderCache, MissThenHitOnSameKey) {
    MessageRenderCache c;
    c.resize(2);
    MessageRenderCacheKey k1{42, 100, 1u, true};
    c.store(0, k1, text("a"), {});
    ASSERT_NE(c.element(0), nullptr);
    EXPECT_TRUE(c.valid(0, k1));
    EXPECT_FALSE(c.valid(0, MessageRenderCacheKey{43, 100, 1u, true}));
    EXPECT_FALSE(c.valid(0, MessageRenderCacheKey{42, 99, 1u, true}));
    EXPECT_FALSE(c.valid(0, MessageRenderCacheKey{42, 100, 2u, true}));
    EXPECT_FALSE(c.valid(0, MessageRenderCacheKey{42, 100, 1u, false}));
}

TEST(MessageRenderCache, InvalidatesSingleAndAll) {
    MessageRenderCache c;
    c.resize(3);
    c.store(0, {1, 100, 1u, true}, text("x"), {});
    c.store(1, {1, 100, 1u, true}, text("y"), {});
    c.invalidate(0);
    EXPECT_FALSE(c.valid(0, {1, 100, 1u, true}));
    EXPECT_TRUE(c.valid(1, {1, 100, 1u, true}));
    c.invalidate_all();
    EXPECT_FALSE(c.valid(1, {1, 100, 1u, true}));
}

TEST(MessageRenderCache, StoresAndReplaysLinkRegions) {
    MessageRenderCache c;
    c.resize(1);
    c.store(0, {1, 100, 1u, true}, text("x"), {{"https://a.b", 2, 3, 4, 5}});
    const auto& lr = c.link_regions(0);
    ASSERT_EQ(lr.size(), 1u);
    EXPECT_EQ(lr[0].href, "https://a.b");
    EXPECT_EQ(lr[0].x, 2);
    EXPECT_EQ(lr[0].y, 3);
}

TEST(MessageRenderCache, StoreGrowsForAppendedMessagesWithoutDroppingEarlierEntries) {
    MessageRenderCache c;
    const MessageRenderCacheKey first_key{1, 80, 1u, true};
    const MessageRenderCacheKey second_key{2, 80, 1u, true};

    // A new conversation starts with zero cache slots. Storing its first
    // message must not be silently discarded.
    c.store(0, first_key, text("first"), {});
    EXPECT_TRUE(c.valid(0, first_key));

    // Appending another message grows storage while preserving the completed
    // message already cached at index zero.
    c.store(1, second_key, text("second"), {});
    EXPECT_TRUE(c.valid(0, first_key));
    EXPECT_TRUE(c.valid(1, second_key));
}

TEST(MessageRenderCache, EnsureSizePreservesButTranscriptResizeResets) {
    MessageRenderCache c;
    const MessageRenderCacheKey key{7, 80, 1u, true};
    c.resize(1);
    c.store(0, key, text("cached"), {});

    c.ensure_size(2);
    EXPECT_TRUE(c.valid(0, key));

    c.resize(2);
    EXPECT_FALSE(c.valid(0, key));
}

} // namespace acecode::tui
