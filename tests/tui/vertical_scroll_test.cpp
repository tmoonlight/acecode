#include <gtest/gtest.h>

#include "tui/vertical_scroll.hpp"

using acecode::tui::clamp_vertical_scroll_top_row;
using acecode::tui::vertical_frame_focus_y_for_scroll_top;
using acecode::tui::vertical_max_scroll_top_row;
using acecode::tui::vertical_scroll_top_row_by_lines;
using acecode::tui::vertical_scrollbar_grab_offset_2x;
using acecode::tui::vertical_scrollbar_thumb_geometry;
using acecode::tui::vertical_scrollbar_y_to_top_row;
using acecode::tui::vertical_scrollbar_y_to_top_row_with_grab;

TEST(VerticalScroll, ClampsLineStepsToTheDocumentRange) {
    EXPECT_EQ(vertical_max_scroll_top_row(100, 20), 80);
    EXPECT_EQ(vertical_max_scroll_top_row(10, 20), 0);
    EXPECT_EQ(clamp_vertical_scroll_top_row(-4, 100, 20), 0);
    EXPECT_EQ(clamp_vertical_scroll_top_row(90, 100, 20), 80);
    EXPECT_EQ(vertical_scroll_top_row_by_lines(12, 3, 100, 20), 15);
    EXPECT_EQ(vertical_scroll_top_row_by_lines(1, -3, 100, 20), 0);
}

TEST(VerticalScroll, ConvertsViewportTopToFtxuiFocusPosition) {
    EXPECT_EQ(vertical_frame_focus_y_for_scroll_top(0, 5), 2);
    EXPECT_EQ(vertical_frame_focus_y_for_scroll_top(15, 5), 17);
    EXPECT_EQ(vertical_frame_focus_y_for_scroll_top(-3, 0), 0);
}

TEST(VerticalScroll, DirectTrackMappingReachesBothEnds) {
    EXPECT_EQ(vertical_scrollbar_y_to_top_row(
                  10, 10, 20, 100, 20),
              0);
    EXPECT_EQ(vertical_scrollbar_y_to_top_row(
                  29, 10, 20, 100, 20),
              80);
    EXPECT_EQ(vertical_scrollbar_y_to_top_row(
                  19, 10, 20, 10, 20),
              0);
}

TEST(VerticalScroll, ThumbDragPreservesTheGrabPointAndClamps) {
    const auto geometry = vertical_scrollbar_thumb_geometry(
        /*track_y_min=*/10,
        /*track_height=*/20,
        /*content_rows=*/100,
        /*viewport_rows=*/20,
        /*scroll_top_row=*/40);
    EXPECT_EQ(geometry.max_top_row, 80);
    EXPECT_GT(geometry.thumb_size_2x, 0);
    EXPECT_GT(geometry.scroll_range_2x, 0);

    const int thumb_middle_y =
        (geometry.thumb_top_2x + geometry.thumb_size_2x / 2) / 2;
    const int grab = vertical_scrollbar_grab_offset_2x(
        thumb_middle_y, geometry);
    const int same_position = vertical_scrollbar_y_to_top_row_with_grab(
        thumb_middle_y, 10, geometry, grab);
    EXPECT_NEAR(same_position, 40, 3);

    EXPECT_EQ(vertical_scrollbar_y_to_top_row_with_grab(
                  -100, 10, geometry, grab),
              0);
    EXPECT_EQ(vertical_scrollbar_y_to_top_row_with_grab(
                  1000, 10, geometry, grab),
              80);
}

TEST(VerticalScroll, FittingContentUsesTheWholeTrackAsThumb) {
    const auto geometry = vertical_scrollbar_thumb_geometry(
        /*track_y_min=*/3,
        /*track_height=*/8,
        /*content_rows=*/6,
        /*viewport_rows=*/8,
        /*scroll_top_row=*/99);
    EXPECT_EQ(geometry.max_top_row, 0);
    EXPECT_EQ(geometry.scroll_range_2x, 0);
    EXPECT_EQ(geometry.thumb_size_2x, 16);
    EXPECT_EQ(vertical_scrollbar_grab_offset_2x(5, geometry), 0);
}
