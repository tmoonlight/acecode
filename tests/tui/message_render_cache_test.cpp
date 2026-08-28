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

} // namespace acecode::tui
