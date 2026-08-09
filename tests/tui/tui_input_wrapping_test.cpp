#include <gtest/gtest.h>

#include "tui/tui_helpers.hpp"
#include "tui/theme_palette.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

const std::string kOpenFullWidthParen = "\xEF\xBC\x88";
const std::string kCloseFullWidthParen = "\xEF\xBC\x89";
const std::string kOpenBookTitle = "\xE3\x80\x8A";
const std::string kCloseBookTitle = "\xE3\x80\x8B";
const std::string kOpenCornerQuote = "\xE3\x80\x8C";
const std::string kCloseCornerQuote = "\xE3\x80\x8D";
const std::string kFullWidthComma = "\xEF\xBC\x8C";
const std::string kCjkA = "\xE7\x94\xB2";
const std::string kCjkB = "\xE4\xB9\x99";
const std::string kCjkC = "\xE4\xB8\x99";

std::string join_tokens(const std::vector<std::string>& tokens) {
    std::string joined;
    for (const auto& token : tokens) {
        joined += token;
    }
    return joined;
}

void render_hit_layout(
    const std::string& input,
    size_t cursor,
    int width,
    ftxui::Box* input_box,
    std::vector<acecode::tui::InputTextHitRegion>* hit_regions,
    std::optional<size_t> selection_anchor = std::nullopt) {
    *input_box = ftxui::Box{0, -1, 0, -1};
    ftxui::Screen screen(width, 8);
    auto element = acecode::tui::render_wrapped_input_text(
        input, cursor, hit_regions, selection_anchor) |
        ftxui::reflect(*input_box);
    ftxui::Render(screen, element);
}

void render_empty_hit_layout(
    int width,
    int height,
    ftxui::Box* input_box,
    std::vector<acecode::tui::InputTextHitRegion>* hit_regions) {
    *input_box = ftxui::Box{0, -1, 0, -1};
    ftxui::Screen screen(width, height);
    auto element = ftxui::hbox({
        ftxui::text(" > "),
        acecode::tui::render_empty_input_prompt(hit_regions) |
            ftxui::flex | ftxui::reflect(*input_box),
    });
    ftxui::Render(screen, element);
}

const acecode::tui::InputTextHitRegion* find_region(
    const std::vector<acecode::tui::InputTextHitRegion>& regions,
    size_t byte_begin,
    size_t byte_end) {
    for (const auto& region : regions) {
        if (region.byte_begin == byte_begin && region.byte_end == byte_end) {
            return &region;
        }
    }
    return nullptr;
}

} // namespace

TEST(TuiInputWrappingTest, PreservesReportedNestedFullWidthParenthesesOrder) {
    const std::string input =
        "1" + kOpenFullWidthParen + kOpenFullWidthParen +
        kCloseFullWidthParen + kCloseFullWidthParen;
    const std::string reversed =
        "1" + kCloseFullWidthParen + kCloseFullWidthParen +
        kOpenFullWidthParen + kOpenFullWidthParen;

    const auto tokens = acecode::tui::tokenize_wrapped_input(input);

    EXPECT_EQ(join_tokens(tokens), input);
    EXPECT_NE(join_tokens(tokens), reversed);
}

TEST(TuiInputWrappingTest, RendersNestedFullWidthParenthesesInAuthoredOrder) {
    const std::string input =
        "1" + kOpenFullWidthParen + kOpenFullWidthParen +
        kCloseFullWidthParen + kCloseFullWidthParen;
    const std::string reversed =
        "1" + kCloseFullWidthParen + kCloseFullWidthParen +
        kOpenFullWidthParen + kOpenFullWidthParen;
    ftxui::Screen screen(12, 1);

    ftxui::Render(
        screen,
        acecode::tui::render_wrapped_input_text(input, input.size()));

    const std::string rendered = screen.ToString();
    EXPECT_EQ(rendered.find(input), 0u) << rendered;
    EXPECT_EQ(rendered.find(reversed), std::string::npos) << rendered;
}

TEST(TuiInputCursorTest, UsesBlockAtUtf8CharacterBoundaries) {
    const std::string input = "A" + kCjkA + "B";

    for (const auto& [cursor_bytes, expected_x] :
         std::vector<std::pair<size_t, int>>{
             {1u, 1},
             {1u + kCjkA.size(), 3},
         }) {
        SCOPED_TRACE(cursor_bytes);
        ftxui::Screen screen(12, 1);

        ftxui::Render(
            screen,
            acecode::tui::render_wrapped_input_text(input, cursor_bytes));

        EXPECT_EQ(screen.cursor().shape, ftxui::Screen::Cursor::Block);
        EXPECT_EQ(screen.cursor().x, expected_x);
        EXPECT_EQ(screen.cursor().y, 0);
        EXPECT_EQ(screen.ToString().find(input), 0u) << screen.ToString();
    }
}

TEST(TuiInputCursorTest, UsesBlockForEmptyPrompt) {
    ftxui::Screen screen(32, 1);

    ftxui::Render(screen, acecode::tui::render_empty_input_prompt());

    EXPECT_EQ(screen.cursor().shape, ftxui::Screen::Cursor::Block);
    EXPECT_EQ(screen.cursor().x, 0);
    EXPECT_EQ(screen.cursor().y, 0);
}

TEST(TuiInputSelectionTest, RecognizesStandardShiftArrowSequences) {
    using Direction = acecode::tui::ShiftArrowDirection;
    EXPECT_EQ(acecode::tui::shift_arrow_direction(
                  ftxui::Event::Special("\x1B[1;2A")),
              Direction::Up);
    EXPECT_EQ(acecode::tui::shift_arrow_direction(
                  ftxui::Event::Special("\x1B[1;2B")),
              Direction::Down);
    EXPECT_EQ(acecode::tui::shift_arrow_direction(
                  ftxui::Event::Special("\x1B[1;2C")),
              Direction::Right);
    EXPECT_EQ(acecode::tui::shift_arrow_direction(
                  ftxui::Event::Special("\x1B[1;2D")),
              Direction::Left);
    EXPECT_FALSE(acecode::tui::shift_arrow_direction(
        ftxui::Event::ArrowLeft).has_value());
}

TEST(TuiInputSelectionTest, HighlightsRangeAndKeepsBlockAtActiveEdge) {
    acecode::tui::init_theme_palette("dark");
    ftxui::Screen screen(8, 1);
    ftxui::Render(
        screen,
        acecode::tui::render_wrapped_input_text(
            "abc", 1, nullptr, std::size_t{3}));

    EXPECT_NE(screen.CellAt(0, 0).background_color,
              acecode::tui::theme().ui.selection_bg);
    EXPECT_EQ(screen.CellAt(1, 0).background_color,
              acecode::tui::theme().ui.selection_bg);
    EXPECT_EQ(screen.CellAt(2, 0).background_color,
              acecode::tui::theme().ui.selection_bg);
    EXPECT_EQ(screen.CellAt(1, 0).foreground_color,
              acecode::tui::theme().ui.selection_fg);
    EXPECT_EQ(screen.cursor().shape, ftxui::Screen::Cursor::Block);
    EXPECT_EQ(screen.cursor().x, 1);
}

TEST(TuiInputSelectionTest, FindsVerticalTargetOnWrappedVisualRow) {
    const std::string input = "one two three";
    ftxui::Box input_box;
    std::vector<acecode::tui::InputTextHitRegion> regions;
    render_hit_layout(input, 1, 7, &input_box, &regions);
    std::optional<int> goal_column;

    const auto target = acecode::tui::input_cursor_vertical_target(
        input,
        input_box,
        regions,
        1,
        acecode::tui::ShiftArrowDirection::Down,
        &goal_column);

    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(*target, 5u);
    EXPECT_EQ(goal_column, 1);
}

TEST(TuiInputSelectionTest, VerticalTargetClampsToShortWrappedRowEnd) {
    const std::string input = "abcdef x";
    ftxui::Box input_box;
    std::vector<acecode::tui::InputTextHitRegion> regions;
    render_hit_layout(input, 5, 7, &input_box, &regions);
    std::optional<int> goal_column;

    const auto target = acecode::tui::input_cursor_vertical_target(
        input,
        input_box,
        regions,
        5,
        acecode::tui::ShiftArrowDirection::Down,
        &goal_column);

    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(*target, input.size());
    EXPECT_EQ(goal_column, 5);
}

TEST(TuiInputWrappingTest, ReconstructsMixedAndBoundaryInputsByteForByte) {
    const std::vector<std::string> inputs = {
        "",
        "plain ASCII",
        "  leading and trailing  ",
        kOpenFullWidthParen + kCloseFullWidthParen,
        "A" + kOpenFullWidthParen,
        kCloseFullWidthParen + "B",
        "A" + kOpenFullWidthParen + " " + "B" +
            kCloseFullWidthParen + "C",
        kCjkA + kOpenBookTitle + kOpenCornerQuote + kCjkB +
            kCloseCornerQuote + kCloseBookTitle + kCjkC,
    };

    for (const auto& input : inputs) {
        SCOPED_TRACE(input);
        EXPECT_EQ(join_tokens(acecode::tui::tokenize_wrapped_input(input)),
                  input);
    }
}

TEST(TuiInputWrappingTest, KeepsOpeningAndClosingPunctuationWithContent) {
    const auto parenthesized = acecode::tui::tokenize_wrapped_input(
        kCjkA + kOpenFullWidthParen + kCjkB + kCloseFullWidthParen + kCjkC);
    ASSERT_EQ(parenthesized.size(), 3u);
    EXPECT_EQ(parenthesized[0], kCjkA);
    EXPECT_EQ(parenthesized[1],
              kOpenFullWidthParen + kCjkB + kCloseFullWidthParen);
    EXPECT_EQ(parenthesized[2], kCjkC);

    const auto comma = acecode::tui::tokenize_wrapped_input(
        kCjkA + kFullWidthComma + kCjkB);
    ASSERT_EQ(comma.size(), 2u);
    EXPECT_EQ(comma[0], kCjkA + kFullWidthComma);
    EXPECT_EQ(comma[1], kCjkB);
}

TEST(TuiInputPointerTest, MapsAsciiCellsAndTrailingRowSpace) {
    const std::string input = "abc";
    ftxui::Box input_box;
    std::vector<acecode::tui::InputTextHitRegion> regions;
    render_hit_layout(input, input.size(), 8, &input_box, &regions);

    const auto* text_region = find_region(regions, 0, input.size());
    ASSERT_NE(text_region, nullptr);
    EXPECT_EQ(acecode::tui::input_cursor_from_point(
                  input, input_box, regions,
                  text_region->box.x_min, text_region->box.y_min),
              0u);
    EXPECT_EQ(acecode::tui::input_cursor_from_point(
                  input, input_box, regions,
                  text_region->box.x_min + 1, text_region->box.y_min),
              1u);
    EXPECT_EQ(acecode::tui::input_cursor_from_point(
                  input, input_box, regions,
                  input_box.x_max, text_region->box.y_min),
              input.size());
}

TEST(TuiInputPointerTest, MapsBothCellsOfFullWidthGlyphToUtf8Boundaries) {
    const std::string input = "A" + kCjkA + "B";
    ftxui::Box input_box;
    std::vector<acecode::tui::InputTextHitRegion> regions;
    render_hit_layout(input, 1, 10, &input_box, &regions);

    const auto* cjk_region = find_region(regions, 1, 1 + kCjkA.size());
    ASSERT_NE(cjk_region, nullptr);
    ASSERT_EQ(cjk_region->box.x_max - cjk_region->box.x_min + 1, 2);
    EXPECT_EQ(acecode::tui::input_cursor_from_point(
                  input, input_box, regions,
                  cjk_region->box.x_min, cjk_region->box.y_min),
              1u);
    EXPECT_EQ(acecode::tui::input_cursor_from_point(
                  input, input_box, regions,
                  cjk_region->box.x_min + 1, cjk_region->box.y_min),
              1u + kCjkA.size());
}

TEST(TuiInputPointerTest, UsesActualWrappedRowAndItsTrailingBoundary) {
    const std::string input = "one two three";
    ftxui::Box input_box;
    std::vector<acecode::tui::InputTextHitRegion> regions;
    render_hit_layout(input, input.size(), 7, &input_box, &regions);

    const auto* first_row = find_region(regions, 0, 4);
    const auto* second_row = find_region(regions, 4, 8);
    ASSERT_NE(first_row, nullptr);
    ASSERT_NE(second_row, nullptr);
    ASSERT_GT(second_row->box.y_min, first_row->box.y_min);

    EXPECT_EQ(acecode::tui::input_cursor_from_point(
                  input, input_box, regions,
                  second_row->box.x_min + 1, second_row->box.y_min),
              5u);
    EXPECT_EQ(acecode::tui::input_cursor_from_point(
                  input, input_box, regions,
                  input_box.x_max, first_row->box.y_min),
              4u);
}

TEST(TuiInputPointerTest, HandlesEmptyInputAndRejectsOutsideClick) {
    ftxui::Box input_box;
    std::vector<acecode::tui::InputTextHitRegion> regions;
    render_empty_hit_layout(12, 3, &input_box, &regions);

    const auto* empty_row = find_region(regions, 0, 0);
    ASSERT_NE(empty_row, nullptr);
    ASSERT_LT(empty_row->box.y_max, input_box.y_max);
    EXPECT_EQ(acecode::tui::input_cursor_from_point(
                  "", input_box, regions,
                  input_box.x_max, empty_row->box.y_min),
              0u);
    EXPECT_FALSE(acecode::tui::input_cursor_from_point(
        "", input_box, regions,
        input_box.x_min, empty_row->box.y_max + 1).has_value());
    EXPECT_FALSE(acecode::tui::input_cursor_from_point(
        "", input_box, regions,
        input_box.x_min - 1, empty_row->box.y_min).has_value());

    const std::vector<acecode::tui::InputTextHitRegion> no_regions;
    EXPECT_FALSE(acecode::tui::input_cursor_from_point(
        "", input_box, no_regions,
        input_box.x_min, empty_row->box.y_min).has_value());
}

TEST(TuiInputPointerTest, PlacesCaretWithoutConsumingSelectionPress) {
    const std::string input = "abc";
    acecode::tui::InputTextHitLayout layout;
    render_hit_layout(
        input, input.size(), 8, &layout.box, &layout.regions);
    layout.input_value = input;

    const auto* text_region = find_region(layout.regions, 0, input.size());
    ASSERT_NE(text_region, nullptr);

    acecode::TuiState state;
    state.input_text = input;
    state.input_cursor = input.size();
    state.input_selection_anchor = 0;
    const auto press = acecode::tui::resolve_input_pointer_press(
        state,
        layout,
        text_region->box.x_min + 1,
        text_region->box.y_min);

    EXPECT_TRUE(press.cursor_placed);
    EXPECT_FALSE(press.event_consumed);
    EXPECT_EQ(press.target, acecode::tui::InputPointerTarget::Composer);
    EXPECT_EQ(press.cursor_bytes, 1u);
}

TEST(TuiInputPointerTest, ReflectedBoxExcludesPromptPrefixAndFillsRow) {
    const std::string input = "abc";
    ftxui::Box input_box{0, -1, 0, -1};
    std::vector<acecode::tui::InputTextHitRegion> regions;
    ftxui::Screen screen(12, 3);
    auto element = ftxui::hbox({
        ftxui::text(" > "),
        acecode::tui::render_wrapped_input_text(
            input, input.size(), &regions) |
            ftxui::flex | ftxui::reflect(input_box),
    });
    ftxui::Render(screen, element);

    ASSERT_EQ(input_box.x_min, 3);
    ASSERT_EQ(input_box.x_max, 11);
    EXPECT_FALSE(acecode::tui::input_cursor_from_point(
        input, input_box, regions, 2, input_box.y_min).has_value());
    EXPECT_EQ(acecode::tui::input_cursor_from_point(
                  input, input_box, regions, 3, input_box.y_min),
              0u);
}

TEST(TuiInputPointerTest, EnablesOnlyEditablePromptStates) {
    acecode::TuiState state;
    EXPECT_EQ(acecode::tui::input_pointer_target(state),
              acecode::tui::InputPointerTarget::Composer);

    state.ask_pending = true;
    EXPECT_EQ(acecode::tui::input_pointer_target(state),
              acecode::tui::InputPointerTarget::None);
    state.ask_other_input_active = true;
    EXPECT_EQ(acecode::tui::input_pointer_target(state),
              acecode::tui::InputPointerTarget::AskOther);

    state.confirm_pending = true;
    EXPECT_EQ(acecode::tui::input_pointer_target(state),
              acecode::tui::InputPointerTarget::None);
}
