#include <gtest/gtest.h>

#include "tui/todo_checklist_view.hpp"
#include "tui/tui_helpers.hpp"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

using acecode::DiffHunk;
using acecode::TodoItem;
using acecode::ToolSummary;
using acecode::TuiState;
using ftxui::Box;
using ftxui::Render;
using ftxui::Screen;

namespace {

TuiState::Message changed_message(int index) {
    TuiState::Message message;
    message.role = "tool_result";
    ToolSummary summary;
    summary.object = "src/sidebar_file_" + std::to_string(index) + ".cpp";
    summary.metrics.emplace_back("+", "1");
    summary.metrics.emplace_back("-", "0");
    message.summary = std::move(summary);
    DiffHunk hunk;
    hunk.old_start = 1;
    hunk.old_count = 1;
    hunk.new_start = 1;
    hunk.new_count = 1;
    message.hunks = std::vector<DiffHunk>{hunk};
    return message;
}

void populate_sidebar_state(TuiState& state, bool expanded) {
    state.transcript_expanded = expanded;
    state.current_session_title = "sidebar flow title";
    for (int i = 0; i < 10; ++i) {
        state.mcp_sidebar_servers.push_back({
            "mcp-sidebar-" + std::to_string(i),
            "connected",
            "stdio",
            "",
            2,
        });
    }
    for (int i = 0; i < 12; ++i) {
        state.conversation.push_back(changed_message(i));
        state.todos.push_back(TodoItem{
            std::to_string(i),
            "sidebar todo item " + std::to_string(i),
            i == 0 ? "in_progress" : "pending",
        });
    }
    state.subagent_tasks.push_back({
        "subagent-1",
        "sidebar background task",
        "unused prompt",
        std::chrono::steady_clock::now(),
    });
}

struct SidebarRender {
    Box content_box{1, 0, 1, 0};
    Box viewport_box{1, 0, 1, 0};
    Box scrollbar_box{1, 0, 1, 0};
    std::string text;
    bool painted_scrollbar = false;
};

void render_sidebar(TuiState& state,
                    int height,
                    SidebarRender& output) {
    Screen screen(43, height);
    Render(
        screen,
        acecode::tui::render_regular_sidebar(
            state,
            "acecode sidebar footer",
            "C:\\sidebar\\cwd",
            43,
            0,
            output.content_box,
            output.viewport_box,
            output.scrollbar_box));
    output.text = screen.ToString();
    output.painted_scrollbar = false;
    if (!output.scrollbar_box.IsEmpty()) {
        for (int y = output.scrollbar_box.y_min;
             y <= output.scrollbar_box.y_max;
             ++y) {
            if (screen.CellAt(output.scrollbar_box.x_max, y).character != " ") {
                output.painted_scrollbar = true;
                break;
            }
        }
    }
}

int count_substring(const std::string& text, const std::string& needle) {
    int count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

} // namespace

TEST(RegularSidebar, CompactModeKeepsExistingCapsAndNoViewport) {
    TuiState state;
    populate_sidebar_state(state, false);
    SidebarRender rendered;
    render_sidebar(state, 100, rendered);

    EXPECT_NE(rendered.text.find("+2 more servers"), std::string::npos);
    EXPECT_EQ(count_substring(rendered.text, "+2 more"), 3);
    EXPECT_EQ(rendered.text.find("mcp-sidebar-9"), std::string::npos);
    EXPECT_EQ(rendered.text.find("sidebar_file_11.cpp"), std::string::npos);
    EXPECT_EQ(rendered.text.find("sidebar todo item 11"), std::string::npos);
    EXPECT_TRUE(rendered.content_box.IsEmpty());
    EXPECT_TRUE(rendered.viewport_box.IsEmpty());
    EXPECT_TRUE(rendered.scrollbar_box.IsEmpty());
}

TEST(RegularSidebar, ExpandedModeRendersEveryFoldedItemInFlowOrder) {
    TuiState state;
    populate_sidebar_state(state, true);
    SidebarRender rendered;
    render_sidebar(state, 120, rendered);

    EXPECT_EQ(rendered.text.find("more servers"), std::string::npos);
    EXPECT_EQ(rendered.text.find("+2 more"), std::string::npos);
    EXPECT_NE(rendered.text.find("mcp-sidebar-9"), std::string::npos);
    EXPECT_NE(rendered.text.find("sidebar_file_11.cpp"), std::string::npos);
    EXPECT_NE(rendered.text.find("sidebar todo item 11"), std::string::npos);

    const auto title = rendered.text.find("sidebar flow title");
    const auto mcp = rendered.text.find("MCP");
    const auto files = rendered.text.find("Files Changed");
    const auto todo = rendered.text.find("sidebar todo item 0");
    const auto background = rendered.text.find("Background Tasks");
    const auto footer = rendered.text.find("acecode sidebar footer");
    ASSERT_NE(title, std::string::npos);
    ASSERT_NE(mcp, std::string::npos);
    ASSERT_NE(files, std::string::npos);
    ASSERT_NE(todo, std::string::npos);
    ASSERT_NE(background, std::string::npos);
    ASSERT_NE(footer, std::string::npos);
    EXPECT_LT(title, mcp);
    EXPECT_LT(mcp, files);
    EXPECT_LT(files, todo);
    EXPECT_LT(todo, background);
    EXPECT_LT(background, footer);
}

TEST(RegularSidebar, ExpandedOverflowPublishesAndPaintsIndependentScrollbar) {
    TuiState state;
    populate_sidebar_state(state, true);
    SidebarRender rendered;
    render_sidebar(state, 12, rendered);

    ASSERT_FALSE(rendered.content_box.IsEmpty());
    ASSERT_FALSE(rendered.viewport_box.IsEmpty());
    ASSERT_FALSE(rendered.scrollbar_box.IsEmpty());
    const int content_rows =
        rendered.content_box.y_max - rendered.content_box.y_min + 1;
    const int viewport_rows =
        rendered.viewport_box.y_max - rendered.viewport_box.y_min + 1;
    EXPECT_GT(content_rows, viewport_rows);
    EXPECT_TRUE(rendered.painted_scrollbar);

    state.sidebar_scroll_top_row = 100000;
    render_sidebar(state, 12, rendered);
    EXPECT_EQ(
        state.sidebar_scroll_top_row,
        content_rows - viewport_rows);
}

TEST(RegularSidebar, ExpandedTodoRowsKeepTwoLinePresentation) {
    std::vector<TodoItem> todos = {
        {"1", "This todo is deliberately long enough to wrap beyond two compact sidebar lines", "pending"},
        {"2", "short", "completed"},
        {"3", "active", "in_progress"},
    };
    const auto rows = acecode::tui::todo_checklist_rows(
        todos, /*content_width=*/12, todos.size());

    ASSERT_EQ(rows.size(), todos.size());
    EXPECT_LE(rows[0].content_lines.size(), 2u);
    EXPECT_EQ(rows[0].marker, acecode::tui::todo_filled_square_marker());
    EXPECT_EQ(rows[1].marker, acecode::tui::todo_open_square_marker());
    EXPECT_EQ(rows[2].marker, acecode::tui::todo_check_marker());
}
