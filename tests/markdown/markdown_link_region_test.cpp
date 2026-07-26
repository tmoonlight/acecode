#include <gtest/gtest.h>

#include "markdown/markdown_formatter.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <set>
#include <string>

namespace {

using acecode::markdown::FormatOptions;
using acecode::markdown::MarkdownLinkRegionCollector;
using acecode::markdown::format_markdown;

TEST(MarkdownLinkRegion, RetainsDestinationAndRenderedHitBox) {
    MarkdownLinkRegionCollector links;
    FormatOptions options;
    options.link_regions = &links;

    auto element = format_markdown(
        "[build/acecode.vcxproj](build/acecode.vcxproj)",
        options);
    ftxui::Screen screen(60, 3);
    ftxui::Render(screen, element);

    ASSERT_EQ(links.regions().size(), 1u);
    const auto& region = links.regions().front();
    EXPECT_EQ(region.href, "build/acecode.vcxproj");
    EXPECT_FALSE(region.box.IsEmpty());
    EXPECT_EQ(
        links.href_at(region.box.x_min, region.box.y_min),
        std::optional<std::string>("build/acecode.vcxproj"));
    EXPECT_FALSE(
        links.href_at(region.box.x_max + 1, region.box.y_min).has_value());
}

TEST(MarkdownLinkRegion, EveryWrappedFragmentKeepsTheSameDestination) {
    MarkdownLinkRegionCollector links;
    FormatOptions options;
    options.terminal_width = 8;
    options.link_regions = &links;

    auto element = format_markdown(
        "[alpha beta gamma](src/example.cpp)",
        options);
    ftxui::Screen screen(8, 6);
    ftxui::Render(screen, element);

    ASSERT_EQ(links.regions().size(), 3u);
    std::set<int> rows;
    for (const auto& region : links.regions()) {
        EXPECT_EQ(region.href, "src/example.cpp");
        EXPECT_FALSE(region.box.IsEmpty());
        rows.insert(region.box.y_min);
        EXPECT_EQ(
            links.href_at(region.box.x_min, region.box.y_min),
            std::optional<std::string>("src/example.cpp"));
    }
    EXPECT_GT(rows.size(), 1u);
}

TEST(MarkdownLinkRegion, ClearDropsStaleFrameGeometry) {
    MarkdownLinkRegionCollector links;
    FormatOptions options;
    options.link_regions = &links;

    auto linked = format_markdown("[one](first.txt)", options);
    ftxui::Screen first_screen(20, 2);
    ftxui::Render(first_screen, linked);
    ASSERT_FALSE(links.regions().empty());
    const int old_x = links.regions().front().box.x_min;
    const int old_y = links.regions().front().box.y_min;

    links.clear();
    auto plain = format_markdown("plain text", options);
    ftxui::Screen second_screen(20, 2);
    ftxui::Render(second_screen, plain);

    EXPECT_TRUE(links.regions().empty());
    EXPECT_FALSE(links.href_at(old_x, old_y).has_value());
}

} // namespace
