// 覆盖 TUI AskUserQuestion overlay 的纯排版与滚动数学:
//   - 长 question / option label / description 必须按显示宽度换行;
//   - 中文、混合中英文、长 URL/path token 必须在 UTF-8 边界内换行;
//   - PageUp/PageDown、滚轮、滚动条拖动用到的 offset 计算必须 clamp;
//   - option focus 移动后可以把对应 wrapped row range 滚进 viewport;
//   - Other 输入态继续由 ask_overlay_input helper 隔离,同时 overlay 滚动状态可用。

#include <gtest/gtest.h>

#include "tui/ask_question_overlay.hpp"
#include "tool/ask_overlay_input.hpp"
#include "tui_state.hpp"
#include "utils/encoding.hpp"

#include <ftxui/component/event.hpp>

#include <algorithm>
#include <string>
#include <vector>

using acecode::AskOption;
using acecode::AskQuestion;
using acecode::InputMode;
using acecode::TuiState;
using acecode::try_handle_ask_other_input;
using acecode::tui::AskOverlayLayoutInput;
using acecode::tui::AskOverlayRowKind;
using ftxui::Event;

namespace {

AskQuestion make_question() {
    AskQuestion q;
    q.question = "Which implementation path should we take for this dialog?";
    q.header = "Decision";
    q.options = {
        AskOption{
            "Small patch",
            "Keep the current flow and add only the missing behavior.",
        },
        AskOption{
            "Broader refactor",
            "Extract a reusable overlay model before touching event routing.",
        },
    };
    return q;
}

AskOverlayLayoutInput input_for(const AskQuestion& q, int width) {
    AskOverlayLayoutInput input;
    input.question = &q;
    input.current_question_index = 0;
    input.total_questions = 1;
    input.option_focus = 0;
    input.multi_selected.assign(q.options.size(), false);
    input.content_width = width;
    return input;
}

std::vector<std::string> rows_of_kind(const acecode::tui::AskOverlayLayout& layout,
                                      AskOverlayRowKind kind) {
    std::vector<std::string> out;
    for (const auto& row : layout.rows) {
        if (row.kind == kind) {
            out.push_back(row.text);
        }
    }
    return out;
}

std::string trim_left_spaces(std::string s) {
    const auto pos = s.find_first_not_of(' ');
    if (pos == std::string::npos) return "";
    return s.substr(pos);
}

void init_other_mode_state(TuiState& s) {
    s.ask_pending = true;
    s.ask_input_target = acecode::AskInputTarget::Supplement;
    s.input_text.clear();
    s.input_cursor = 0;
    s.input_mode = InputMode::Normal;
    s.slash_dropdown_active = false;
    s.ask_scroll_offset = 0;
    s.ask_scroll_total_rows = 40;
    s.ask_scroll_visible_rows = 8;
}

} // namespace

// 场景:长 question 在窄 overlay 中自动换行,且每个渲染行都不超过内容宽度。
TEST(AskQuestionOverlayTest, LongQuestionWrapsWithinWidth) {
    AskQuestion q = make_question();
    q.question =
        "This is a deliberately long question that should wrap before it reaches the right edge.";
    auto layout = acecode::tui::build_ask_overlay_layout(input_for(q, 24));

    const auto body_rows = rows_of_kind(layout, AskOverlayRowKind::Body);
    ASSERT_GT(body_rows.size(), 1u);
    for (const auto& row : body_rows) {
        EXPECT_LE(acecode::tui::display_width_cells(row), 24) << row;
    }
}

// 场景:长 option label/description 换行后,续行缩进到 option body 下方。
TEST(AskQuestionOverlayTest, LongOptionContinuationAlignsUnderBody) {
    AskQuestion q = make_question();
    q.options[0].label = "Very long option label";
    q.options[0].description =
        "This description is long enough to force at least one continuation row.";
    auto layout = acecode::tui::build_ask_overlay_layout(input_for(q, 30));

    bool saw_continuation = false;
    for (const auto& row : layout.rows) {
        if (row.kind != AskOverlayRowKind::Option || row.option_index != 0) {
            continue;
        }
        EXPECT_LE(acecode::tui::display_width_cells(row.text), 30) << row.text;
        if (row.continuation) {
            saw_continuation = true;
            EXPECT_GT(row.text.find_first_not_of(' '), 4u);
            EXPECT_FALSE(row.focused);
        }
    }
    EXPECT_TRUE(saw_continuation);
}

// 场景:宽终端进入主栏 + sidebar 双栏布局后,ask overlay 必须按实际主栏
// 宽度换行,不能继续用整屏宽度导致标题和选项在 sidebar 前被裁掉。
TEST(AskQuestionOverlayTest, TwoColumnLayoutWrapsHeaderAndOptionsWithinMainColumn) {
    constexpr int kTerminalWidth = 130;
    constexpr int kSidebarWidth = 43;
    const int single_column_width =
        acecode::tui::ask_overlay_content_width_for_frame(
            kTerminalWidth, 0, false, kSidebarWidth);
    const int two_column_width =
        acecode::tui::ask_overlay_content_width_for_frame(
            kTerminalWidth, 0, true, kSidebarWidth);

    EXPECT_EQ(single_column_width, 120);
    EXPECT_EQ(two_column_width, 76);
    EXPECT_LT(two_column_width, single_column_width);
    // 切换到双栏的第一帧仍可能拿到上一帧 128 列的单栏 reflect box；
    // 当前 sidebar 组合必须立即成为上限，不能等下一帧才开始换行。
    EXPECT_EQ(acecode::tui::ask_overlay_content_width_for_frame(
                  kTerminalWidth, 128, true, kSidebarWidth),
              two_column_width);

    AskQuestion q = make_question();
    q.header =
        "A deliberately long decision title that must wrap inside the narrower main column";
    q.options[0].label =
        "A deliberately long first option label for the two column layout";
    q.options[0].description =
        "Its description must also remain visible instead of being clipped by the sidebar.";

    auto layout = acecode::tui::build_ask_overlay_layout(
        input_for(q, two_column_width));
    int header_rows = 0;
    int first_option_rows = 0;
    for (const auto& row : layout.rows) {
        EXPECT_LE(acecode::tui::display_width_cells(row.text), two_column_width)
            << row.text;
        if (row.kind == AskOverlayRowKind::Header) {
            ++header_rows;
        }
        if (row.kind == AskOverlayRowKind::Option && row.option_index == 0) {
            ++first_option_rows;
        }
    }
    EXPECT_GT(header_rows, 1);
    EXPECT_GT(first_option_rows, 1);
}

// 场景:中文和中英文混排没有空格时也能按 UTF-8 边界换行。
TEST(AskQuestionOverlayTest, ChineseAndMixedTextWrapsOnUtf8Boundaries) {
    AskQuestion q = make_question();
    q.question = u8"这是一个很长的问题没有空格也需要自动换行否则用户看不到右侧内容";
    q.options[0].label = u8"中文选项包含EnglishWords和更多中文内容";
    auto layout = acecode::tui::build_ask_overlay_layout(input_for(q, 18));

    const auto body_rows = rows_of_kind(layout, AskOverlayRowKind::Body);
    ASSERT_GT(body_rows.size(), 1u);
    for (const auto& row : layout.rows) {
        EXPECT_TRUE(acecode::is_valid_utf8(row.text)) << row.text;
        EXPECT_LE(acecode::tui::display_width_cells(row.text), 18) << row.text;
    }
}

// 场景:长 URL/path 一类无空格 token 必须硬换行,不能丢字节或调换顺序。
TEST(AskQuestionOverlayTest, LongUnbrokenTokenHardWrapsWithoutDroppingBytes) {
    AskQuestion q = make_question();
    const std::string token =
        "https://example.test/some/really/long/path/with/no/spaces/or/breaks";
    q.question = token;
    auto layout = acecode::tui::build_ask_overlay_layout(input_for(q, 16));

    std::string reconstructed;
    for (const auto& row : layout.rows) {
        if (row.kind == AskOverlayRowKind::Body) {
            reconstructed += trim_left_spaces(row.text);
            EXPECT_LE(acecode::tui::display_width_cells(row.text), 16) << row.text;
        }
    }
    EXPECT_EQ(reconstructed, token);
}

// 场景:滚动 offset 的 clamp、PageUp/PageDown/滚轮步进和滚动条 Y 映射稳定。
TEST(AskQuestionOverlayTest, ScrollMathClampsAndMapsTrackY) {
    EXPECT_EQ(acecode::tui::clamp_scroll_offset(50, 20, 5), 15);
    EXPECT_EQ(acecode::tui::clamp_scroll_offset(-3, 20, 5), 0);
    EXPECT_EQ(acecode::tui::scroll_offset_by_lines(0, 6, 20, 5), 6);
    EXPECT_EQ(acecode::tui::scroll_offset_by_lines(14, 6, 20, 5), 15);
    EXPECT_EQ(acecode::tui::scroll_offset_by_lines(3, -10, 20, 5), 0);
    EXPECT_EQ(acecode::tui::scroll_offset_for_track_y(0, 0, 10, 100, 20), 0);
    EXPECT_EQ(acecode::tui::scroll_offset_for_track_y(9, 0, 10, 100, 20), 80);
}

// 场景:ask overlay 可见高度只受终端可用高度约束,不再被固定 14 行上限卡住。
// 同时小终端仍保留可操作的最小可见行数。
TEST(AskQuestionOverlayTest, VisibleRowsUseTerminalHeightWithoutFixedFourteenCap) {
    EXPECT_EQ(acecode::tui::ask_overlay_visible_rows_for_terminal(40), 28);
    EXPECT_GT(acecode::tui::ask_overlay_visible_rows_for_terminal(40), 14);
    EXPECT_EQ(acecode::tui::ask_overlay_visible_rows_for_terminal(10), 4);
}

// 场景:问题页 header 在右侧渲染包含提交页的页码与 ASCII 进度指示。
TEST(AskQuestionOverlayTest, QuestionHeaderShowsPageIndicator) {
    AskQuestion q = make_question();
    auto input = input_for(q, 64);
    input.total_questions = 3;
    input.answered_questions = {false, true, false};

    auto layout = acecode::tui::build_ask_overlay_layout(input);
    const auto header_rows = rows_of_kind(layout, AskOverlayRowKind::Header);
    ASSERT_FALSE(header_rows.empty());
    EXPECT_NE(header_rows[0].find("Question 1/3"), std::string::npos);
    EXPECT_NE(header_rows[0].find("1/4 [#*.S]"), std::string::npos);
    EXPECT_LE(acecode::tui::display_width_cells(header_rows[0]), 64);
}

// 场景:提交页复用 ask overlay layout,只提供 Submit answers 和 Cancel 两个选择。
TEST(AskQuestionOverlayTest, SubmitPageShowsReadyPromptAndTwoChoices) {
    AskOverlayLayoutInput input;
    input.submit_page = true;
    input.total_questions = 3;
    input.submit_focus = 1;
    input.answered_questions = {true, true, true};
    input.content_width = 64;

    auto layout = acecode::tui::build_ask_overlay_layout(input);
    const auto header_rows = rows_of_kind(layout, AskOverlayRowKind::Header);
    ASSERT_FALSE(header_rows.empty());
    EXPECT_NE(header_rows[0].find("Submit"), std::string::npos);
    EXPECT_NE(header_rows[0].find("4/4 [***#]"), std::string::npos);

    const auto body_rows = rows_of_kind(layout, AskOverlayRowKind::Body);
    ASSERT_FALSE(body_rows.empty());
    EXPECT_NE(body_rows[0].find("Ready to submit?"), std::string::npos);

    const auto option_rows = rows_of_kind(layout, AskOverlayRowKind::Option);
    ASSERT_EQ(option_rows.size(), 2u);
    EXPECT_NE(option_rows[0].find("1. Submit answers"), std::string::npos);
    EXPECT_NE(option_rows[1].find("2. Cancel"), std::string::npos);
    EXPECT_EQ(option_rows[0].find("Other"), std::string::npos);
    EXPECT_EQ(option_rows[1].find("Other"), std::string::npos);
    EXPECT_EQ(layout.focused_row_begin, layout.focused_row_end);
    ASSERT_GE(layout.focused_row_begin, 0);
    EXPECT_EQ(layout.rows[layout.focused_row_begin].option_index, 1);
}

// 场景:焦点移动到当前 viewport 外时,ensure helper 把 wrapped option 行滚进来。
TEST(AskQuestionOverlayTest, FocusRangeIsKeptVisible) {
    EXPECT_EQ(acecode::tui::ensure_row_range_visible(0, 5, 20, 12, 14), 10);
    EXPECT_EQ(acecode::tui::ensure_row_range_visible(10, 5, 20, 2, 3), 2);
    EXPECT_EQ(acecode::tui::ensure_row_range_visible(4, 5, 20, 5, 7), 4);

    AskQuestion q = make_question();
    q.options[1].description =
        "A long focused option description that spans several rendered rows.";
    auto input = input_for(q, 24);
    input.option_focus = 1;
    auto layout = acecode::tui::build_ask_overlay_layout(input);
    ASSERT_GE(layout.focused_row_begin, 0);
    ASSERT_GE(layout.focused_row_end, layout.focused_row_begin);
}

// 场景:Other 输入态字符编辑仍然隔离,同时 ask overlay 的 PageDown 数学可用。
TEST(AskQuestionOverlayTest, OtherInputEditingCoexistsWithOverlayScrollState) {
    TuiState s;
    init_other_mode_state(s);

    EXPECT_TRUE(try_handle_ask_other_input(s, Event::Character("/")));
    EXPECT_EQ(s.input_text, "/");
    EXPECT_FALSE(s.slash_dropdown_active);
    EXPECT_EQ(s.input_mode, InputMode::Normal);

    s.ask_scroll_offset = acecode::tui::scroll_offset_by_lines(
        s.ask_scroll_offset, s.ask_scroll_visible_rows - 1,
        s.ask_scroll_total_rows, s.ask_scroll_visible_rows);
    EXPECT_GT(s.ask_scroll_offset, 0);
    EXPECT_EQ(s.input_text, "/");
}

// 场景(add-ask-question-policy):timeout 策略下 overlay 追加静态提示行
// 「Ns 无操作将自动选择推荐项」;ask 策略(timeout_hint_seconds=0)不出现。
TEST(AskQuestionOverlayTest, TimeoutHintRowRenderedOnlyWhenConfigured) {
    AskQuestion q = make_question();

    auto input = input_for(q, 80);
    input.timeout_hint_seconds = 60;
    auto layout = acecode::tui::build_ask_overlay_layout(input);
    bool found = false;
    for (const auto& row : rows_of_kind(layout, AskOverlayRowKind::Hint)) {
        if (row.find("60s") != std::string::npos &&
            row.find("\xE8\x87\xAA\xE5\x8A\xA8\xE9\x80\x89\xE6\x8B\xA9") !=
                std::string::npos) { // "自动选择"
            found = true;
        }
    }
    EXPECT_TRUE(found);

    auto input_no_hint = input_for(q, 80);
    auto layout_no_hint = acecode::tui::build_ask_overlay_layout(input_no_hint);
    for (const auto& row : rows_of_kind(layout_no_hint, AskOverlayRowKind::Hint)) {
        EXPECT_EQ(row.find("\xE8\x87\xAA\xE5\x8A\xA8\xE9\x80\x89\xE6\x8B\xA9"),
                  std::string::npos);
    }
}

// 场景:鼠标点击选项行命中测试(add-tui-ask-overlay-mouse-select)。
// row_boxes 按可见顺序排列,layout row 由 scroll_offset + 可见下标还原。
TEST(AskQuestionOverlayTest, HitOptionMapsVisibleRowToOptionIndex) {
    using ftxui::Box;
    // 可见 4 行:header / option0 / option1 / hint,scroll_offset=0。
    const std::vector<Box> boxes = {
        Box{2, 40, 10, 10},
        Box{2, 40, 11, 11},
        Box{2, 40, 12, 12},
        Box{2, 40, 13, 13},
    };
    const std::vector<int> indices = {-1, 0, 1, -1};

    EXPECT_EQ(acecode::tui::ask_overlay_hit_option(boxes, 0, indices, 5, 11),
              0);
    EXPECT_EQ(acecode::tui::ask_overlay_hit_option(boxes, 0, indices, 20, 12),
              1);
    EXPECT_EQ(acecode::tui::ask_overlay_hit_option(boxes, 0, indices, 5, 10),
              -1);
    EXPECT_EQ(acecode::tui::ask_overlay_hit_option(boxes, 0, indices, 5, 13),
              -1);
    EXPECT_EQ(acecode::tui::ask_overlay_hit_option(boxes, 0, indices, 0, 11),
              -1);
}

// 场景:滚动后可见行对应 layout 后半段,点击仍映射到正确 option。
TEST(AskQuestionOverlayTest, HitOptionHonorsScrollOffset) {
    using ftxui::Box;
    // 整页 6 行,可见后 3 行(offset=3):option1 / Other / hint。
    const std::vector<Box> boxes = {
        Box{2, 40, 20, 20},
        Box{2, 40, 21, 21},
        Box{2, 40, 22, 22},
    };
    const std::vector<int> indices = {-1, -1, 0, 1, 2, -1};

    EXPECT_EQ(acecode::tui::ask_overlay_hit_option(boxes, 3, indices, 4, 20),
              1);
    EXPECT_EQ(acecode::tui::ask_overlay_hit_option(boxes, 3, indices, 4, 21),
              2);
    EXPECT_EQ(acecode::tui::ask_overlay_hit_option(boxes, 3, indices, 4, 22),
              -1);
    EXPECT_EQ(acecode::tui::ask_overlay_hit_option(boxes, 3, indices, 4, 19),
              -1);
}

// 场景:空 box / 越界 scroll_offset 不崩溃,返回 -1。
TEST(AskQuestionOverlayTest, HitOptionReturnsMinusOneWhenUnmapped) {
    using ftxui::Box;
    const std::vector<Box> empty_boxes;
    const std::vector<int> indices = {0, 1};
    EXPECT_EQ(
        acecode::tui::ask_overlay_hit_option(empty_boxes, 0, indices, 0, 0),
        -1);

    const std::vector<Box> boxes = {Box{0, 10, 0, 0}};
    EXPECT_EQ(acecode::tui::ask_overlay_hit_option(boxes, 5, indices, 1, 0),
              -1);
}

// 双入口布局(ask-user-question-dual-entry,任务 2.4):
// 可聚焦 Option 行 = option_count + 2(预设 N + 「以上都不是」行 N +
// 补充说明内容行 N+1);focus 越界 clamp 到最后一行;独占行在补充区之后。
TEST(AskQuestionOverlayTest, DualEntryAddsExclusiveAndSupplementFocusRows) {
    AskQuestion q = make_question(); // 2 options
    AskOverlayLayoutInput input = input_for(q, 80);
    input.option_focus = 999; // 越界 → clamp 到 total_rows - 1 = option_count + 1
    const auto layout = acecode::tui::build_ask_overlay_layout(input);

    const auto option_rows = rows_of_kind(layout, AskOverlayRowKind::Option);
    // 注意:description 过长时选项可能 wrap 成多渲染行,因此按 option_index
    // 去重后断言可聚焦行集合 = N + 2(预设 0..N-1 + 独占 N + 补充 N+1)。
    std::vector<bool> seen_index(q.options.size() + 2u, false);
    for (const auto& row : layout.rows) {
        if (row.kind != AskOverlayRowKind::Option) continue;
        if (row.option_index >= 0 &&
            row.option_index < static_cast<int>(seen_index.size())) {
            seen_index[static_cast<std::size_t>(row.option_index)] = true;
        }
    }
    int distinct_indices = 0;
    for (const bool s : seen_index) {
        if (s) ++distinct_indices;
    }
    ASSERT_EQ(distinct_indices, static_cast<int>(q.options.size()) + 2);
    ASSERT_FALSE(option_rows.empty());

    // 独占行:勾选标记 + "None of the above" 静态文案(grill Q3),激活文本行内追加。
    // 直接扫 Option 行找独占 / 补充内容行。
    int seen_exclusive = 0;
    int seen_supplement = 0;
    for (const auto& row : layout.rows) {
        if (row.kind != AskOverlayRowKind::Option) continue;
        if (row.option_index == static_cast<int>(q.options.size())) {
            ++seen_exclusive;
            // 独占行文案可能被 wrap;focused 前缀(▸/勾选)存在时仍应含静态文案。
            EXPECT_NE(trim_left_spaces(row.text).find("None of the above"),
                      std::string::npos)
                << row.text;
        }
        if (row.option_index == static_cast<int>(q.options.size()) + 1) {
            ++seen_supplement;
            // focus clamp 到补充行时会带 ▸ 前缀,这里只断言占位文案存在。
            EXPECT_NE(trim_left_spaces(row.text).find("(enter to add a note)"),
                      std::string::npos)
                << row.text;
        }
    }
    EXPECT_EQ(seen_exclusive, 1);
    EXPECT_EQ(seen_supplement, 1);

    // 越界 focus clamp 到最后一个可聚焦行(补充内容行,option_index = N+1)。
    ASSERT_GE(layout.focused_row_begin, 0);
    EXPECT_EQ(layout.rows[layout.focused_row_begin].option_index,
              static_cast<int>(q.options.size()) + 1);
}

// 独占激活(ask-user-question-dual-entry):预设行与补充说明区 dim,
// 独占行本身不 dim、行内展示激活文本;预设行 dim 时内容仍保留。
TEST(AskQuestionOverlayTest, ExclusiveActiveDimsPresetsAndSupplementArea) {
    AskQuestion q = make_question();
    AskOverlayLayoutInput input = input_for(q, 80);
    input.exclusive_active = true;
    input.exclusive_text = "Write our own";
    const auto layout = acecode::tui::build_ask_overlay_layout(input);

    bool exclusive_row_dim = false;
    // 预设行按 option_index 覆盖:每个预设的所有渲染行都必须 dim
    // (description 可能 wrap 成多行,不能按渲染行数断言)。
    std::vector<int> preset_rows_by_index(q.options.size(), 0);
    std::vector<int> preset_dim_by_index(q.options.size(), 0);
    int supplement_rows_dim = 0;
    int supplement_rows_total = 0;
    bool exclusive_text_shown = false;
    for (const auto& row : layout.rows) {
        if (row.kind != AskOverlayRowKind::Option) continue;
        const int n = static_cast<int>(q.options.size());
        if (row.option_index == n) {
            exclusive_row_dim = row.dim;
            if (row.text.find("Write our own") != std::string::npos) {
                exclusive_text_shown = true;
            }
        } else if (row.option_index == n + 1) {
            ++supplement_rows_total;
            if (row.dim) ++supplement_rows_dim;
        } else if (row.option_index >= 0 && row.option_index < n) {
            ++preset_rows_by_index[row.option_index];
            if (row.dim) ++preset_dim_by_index[row.option_index];
        }
    }
    EXPECT_FALSE(exclusive_row_dim);            // 独占行本身不高亮弱化
    EXPECT_TRUE(exclusive_text_shown);          // 激活文本行内展示
    for (std::size_t i = 0; i < q.options.size(); ++i) {
        EXPECT_GT(preset_rows_by_index[i], 0) << "preset " << i << " 应有渲染行";
        EXPECT_EQ(preset_dim_by_index[i], preset_rows_by_index[i])
            << "preset " << i << " 的所有渲染行都应 dim";
    }
    EXPECT_GT(supplement_rows_total, 0);
    EXPECT_EQ(supplement_rows_dim, supplement_rows_total); // 补充内容行 dim(被压制)
}

// 双入口未激活:预设行不 dim;补充文本渲染在补充内容行。
TEST(AskQuestionOverlayTest, SupplementTextRenderedWhenNoExclusive) {
    AskQuestion q = make_question();
    AskOverlayLayoutInput input = input_for(q, 80);
    input.supplement_text = "remember Windows CI";
    const auto layout = acecode::tui::build_ask_overlay_layout(input);

    const int n = static_cast<int>(q.options.size());
    bool supplement_text_shown = false;
    int preset_rows_dim = 0;
    for (const auto& row : layout.rows) {
        if (row.kind != AskOverlayRowKind::Option) continue;
        if (row.option_index == n + 1) {
            EXPECT_FALSE(row.dim);
            if (row.text.find("remember Windows CI") != std::string::npos) {
                supplement_text_shown = true;
            }
        } else if (row.option_index >= 0 && row.option_index < n && row.dim) {
            ++preset_rows_dim;
        }
    }
    EXPECT_TRUE(supplement_text_shown);
    EXPECT_EQ(preset_rows_dim, 0); // 未独占 → 预设行不弱化
}

// 输入态提示 + 离开校验错误行(grill Q1/Q3):
//   input_target=Exclusive → CustomPrompt 提示独占作答;Supplement → 补充提示;
//   validation_error 非空 → Hint 行渲染错误文案。
TEST(AskQuestionOverlayTest, InputTargetPromptAndValidationErrorRows) {
    AskQuestion q = make_question();

    AskOverlayLayoutInput exclusive_input = input_for(q, 80);
    exclusive_input.input_target = 2; // AskInputTarget::Exclusive
    const auto exclusive_layout =
        acecode::tui::build_ask_overlay_layout(exclusive_input);
    const auto exclusive_prompts =
        rows_of_kind(exclusive_layout, AskOverlayRowKind::CustomPrompt);
    ASSERT_EQ(exclusive_prompts.size(), 1u);
    EXPECT_NE(exclusive_prompts[0].find("None of the above - your answer"),
              std::string::npos);

    AskOverlayLayoutInput supplement_input = input_for(q, 80);
    supplement_input.input_target = 1; // AskInputTarget::Supplement
    const auto supplement_layout =
        acecode::tui::build_ask_overlay_layout(supplement_input);
    const auto supplement_prompts =
        rows_of_kind(supplement_layout, AskOverlayRowKind::CustomPrompt);
    ASSERT_EQ(supplement_prompts.size(), 1u);
    EXPECT_NE(supplement_prompts[0].find("Supplement note (Enter to submit"),
              std::string::npos);

    AskOverlayLayoutInput error_input = input_for(q, 80);
    error_input.validation_error =
        "None of the above requires an answer (type it or untoggle with Space).";
    const auto error_layout = acecode::tui::build_ask_overlay_layout(error_input);
    const auto hint_rows = rows_of_kind(error_layout, AskOverlayRowKind::Hint);
    bool found_error = false;
    for (const auto& h : hint_rows) {
        if (h.find("requires an answer") != std::string::npos) {
            found_error = true;
        }
    }
    EXPECT_TRUE(found_error);
}
