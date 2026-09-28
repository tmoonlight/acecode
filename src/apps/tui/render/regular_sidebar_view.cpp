#include "tui/render/regular_sidebar_view.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include <array>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>

#include "tui/tui_state.hpp"
#include "tui/text_style.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_truncation.hpp"
#include "tui/sidebar_model.hpp"
#include "tui/non_selectable.hpp"
#include "tui/pending_attachment_selection.hpp"
#include "tui/thick_vscroll_bar.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/todo_checklist_view.hpp"
#include "tui/unclipped_reflect.hpp"
#include "tui/vertical_scroll.hpp"
#include "session/token_tracker.hpp"
#include "tui/text_input_ops.hpp"
#include "tool/mcp_manager.hpp"
#include "lsp/lsp_service.hpp"

using namespace ftxui;

#include "tui/render/text_cells.hpp"
#include "tui/render/status_chips.hpp"
#include "tui/model/mcp_sidebar_model.hpp"

namespace acecode::tui {
static std::string first_user_message_title(const TuiState& state) {
    std::string explicit_title =
        collapse_sidebar_title_whitespace(state.current_session_title);
    if (!explicit_title.empty()) return explicit_title;
    for (const auto& msg : state.conversation) {
        if (msg.role == "user") {
            std::string title = collapse_sidebar_title_whitespace(msg.content);
            if (!title.empty()) {
                return title;
            }
        }
    }
    return std::string("New session");
}

static Element sidebar_section_header(const std::string& label, int count) {
    return hbox({
        text(label) | readable_secondary(),
        text(" " + std::to_string(count)) | readable_secondary(),
    });
}

static std::string sidebar_change_stats_text(
    const SidebarFileChange& change) {
    std::string out;
    if (change.additions > 0) {
        out += "+" + std::to_string(change.additions);
    }
    if (change.deletions > 0) {
        if (!out.empty()) {
            out += " ";
        }
        out += "-" + std::to_string(change.deletions);
    }
    return out.empty() ? std::string("0") : out;
}

static Element render_sidebar_change_row(
    const SidebarFileChange& change,
    int content_width) {
    const std::string stats_text = sidebar_change_stats_text(change);
    const int file_width =
        std::max(1, content_width - 2 - static_cast<int>(stats_text.size()) - 1);
    Elements stats_parts;
    if (change.additions > 0) {
        stats_parts.push_back(
            text("+" + std::to_string(change.additions)) |
            color(theme().semantic.success));
    }
    if (change.deletions > 0) {
        if (!stats_parts.empty()) {
            stats_parts.push_back(text(" "));
        }
        stats_parts.push_back(
            text("-" + std::to_string(change.deletions)) |
            color(theme().semantic.error));
    }
    if (stats_parts.empty()) {
        stats_parts.push_back(text("0") | readable_secondary());
    }

    return hbox({
        text("  ") | color(theme().ui.text_dim),
        text(truncate_cells_middle_ascii(
                 change.display_file.empty() ? change.file : change.display_file,
                 file_width)) |
            color(theme().ui.text_muted),
        filler(),
        hbox(std::move(stats_parts)),
    });
}

static Element render_mcp_sidebar_section(
    const std::vector<TuiState::McpSidebarServer>& servers,
    int content_width,
    int anim_tick,
    std::size_t max_servers) {
    if (servers.empty()) {
        return emptyElement();
    }

    Elements rows;
    rows.push_back(text("MCP") | bold | color(theme().ui.text_primary));

    std::size_t shown_servers = 0;

    for (const auto& server : servers) {
        if (shown_servers >= max_servers) {
            break;
        }
        ++shown_servers;

        const Color state_color = mcp_sidebar_state_color(server.state);
        const bool server_loading = server.state == "starting";
        const bool server_connected = server.state == "connected";
        const bool server_failed =
            server.state == "failed" || server.state == "timed_out";
        const std::string bullet = "\xE2\x80\xA2";

        Element status;
        if (server_loading) {
            status = render_white_shimmer_text("Loading", anim_tick);
        } else if (server_connected) {
            status = text("Connected (" + format_tool_count(server.tool_count) + ")") |
                     color(theme().ui.text_muted);
        } else if (server_failed && !server.error.empty()) {
            status = hbox({
                text(uppercase_ascii(server.transport) + " error: ") |
                    color(theme().semantic.error),
                paragraph(server.error) |
                    color(theme().ui.text_muted) | dim | flex,
            });
        } else {
            status = text(server.state) | color(state_color);
        }

        const int name_width = std::max(1, content_width / 2);
        rows.push_back(hbox({
            text("  " + bullet + " ") | color(state_color),
            text(truncate_cells_middle_ascii(server.name, name_width)) |
                bold | color(theme().ui.text_primary),
            text(" "),
            status | flex,
        }));
    }

    if (servers.size() > shown_servers) {
        rows.push_back(
            text("  +" + std::to_string(servers.size() - shown_servers) +
                 " more servers") |
            readable_secondary());
    }

    return vbox(std::move(rows));
}

static std::vector<std::string> sidebar_title_lines(
    const std::string& title,
    int max_width) {
    max_width = std::max(1, max_width);
    const auto glyphs = Utf8ToGlyphs(title);
    std::vector<std::string> lines;
    std::size_t index = 0;

    for (int line_index = 0; line_index < 2 && index < glyphs.size(); ++line_index) {
        std::string line;
        int width = 0;
        while (index < glyphs.size()) {
            const auto& glyph = glyphs[index];
            const int glyph_width = std::max(0, string_width(glyph));
            if (width > 0 && width + glyph_width > max_width) {
                break;
            }
            if (width == 0 && glyph_width > max_width) {
                line += glyph;
                ++index;
                break;
            }
            line += glyph;
            width += glyph_width;
            ++index;
        }
        trim_ascii_space_suffix(line);
        lines.push_back(std::move(line));
        while (index < glyphs.size() && glyphs[index] == " ") {
            ++index;
        }
    }

    if (lines.empty()) {
        lines.push_back("New session");
    }
    if (index < glyphs.size()) {
        if (lines.size() == 1) {
            lines.push_back("");
        }
        const int body_width = std::max(0, max_width - 3);
        lines[1] = truncate_cells_prefix(lines[1], body_width);
        trim_ascii_space_suffix(lines[1]);
        lines[1] += "...";
    }
    return lines;
}

Element render_regular_sidebar(TuiState& state,
                               const std::string& version_str,
                               const std::string& cwd_display,
                               int sidebar_width,
                               int anim_tick,
                               Box& content_box,
                               Box& viewport_box,
                               Box& scrollbar_box) {
    constexpr std::size_t kMaxCompactMcpServers = 8;
    constexpr std::size_t kMaxCompactSidebarFiles = 10;
    constexpr int kExpandedScrollbarWidth = 2;
    const bool expanded = state.transcript_expanded;
    const int horizontal_chrome =
        expanded ? 1 + kExpandedScrollbarWidth : 2;
    const int content_width =
        std::max(1, sidebar_width - horizontal_chrome);
    Elements top_rows;
    for (const auto& line : sidebar_title_lines(first_user_message_title(state),
                                                content_width)) {
        top_rows.push_back(text(line) | bold | color(theme().ui.text_primary));
    }

    Element mcp_section = render_mcp_sidebar_section(
        state.mcp_sidebar_servers, content_width, anim_tick,
        expanded ? state.mcp_sidebar_servers.size()
                 : kMaxCompactMcpServers);
    if (!state.mcp_sidebar_servers.empty()) {
        top_rows.push_back(text(""));
        top_rows.push_back(std::move(mcp_section));
    }

    // LSP 状态节(openspec add-lsp-service):有已连接 server 才渲染。
    // connected_snapshot 只做锁 + 小拷贝,每帧调用安全(不做 which 探测)。
    if (lsp::is_initialized()) {
        const auto lsp_servers = lsp::service().connected_snapshot();
        if (!lsp_servers.empty()) {
            top_rows.push_back(text(""));
            top_rows.push_back(sidebar_section_header(
                "LSP", static_cast<int>(lsp_servers.size())));
            for (const auto& server : lsp_servers) {
                std::string row = server.server_id + " (" +
                                  std::to_string(server.open_files) + " files)";
                top_rows.push_back(
                    text("  ● " + truncate_cells_middle_ascii(
                                      row, std::max(1, content_width - 4))) |
                    color(theme().semantic.success));
            }
        }
    }

    const auto file_changes =
        collect_sidebar_file_changes(state.conversation, cwd_display);
    top_rows.push_back(text(""));
    top_rows.push_back(sidebar_section_header(
        "Files Changed", static_cast<int>(file_changes.size())));

    const std::size_t shown_files =
        expanded ? file_changes.size()
                 : std::min(kMaxCompactSidebarFiles, file_changes.size());
    for (std::size_t i = 0; i < shown_files; ++i) {
        top_rows.push_back(
            render_sidebar_change_row(file_changes[i], content_width));
    }
    if (file_changes.size() > shown_files) {
        top_rows.push_back(
            text("  +" + std::to_string(file_changes.size() - shown_files) +
                 " more") |
            readable_secondary());
    }

    Elements bottom_rows;
    if (!state.todos.empty()) {
        bottom_rows.push_back(
            render_todo_checklist_block(
                state.todos, content_width,
                expanded ? state.todos.size()
                         : kTodoChecklistMaxVisibleItems));
        bottom_rows.push_back(text(""));
    }
    const bool show_bash_task =
        state.tool_running && state.tool_progress.tool_name == "bash";
    const auto& subagents = state.subagent_tasks;
    if (show_bash_task || !subagents.empty()) {
        const int task_count =
            (show_bash_task ? 1 : 0) + static_cast<int>(subagents.size());
        bottom_rows.push_back(
            sidebar_section_header("Background Tasks", task_count));
        if (show_bash_task) {
            std::string command = state.tool_progress.command_preview.empty()
                ? std::string("bash")
                : state.tool_progress.command_preview;
            bottom_rows.push_back(
                text("  " + truncate_cells_middle_ascii(
                                 command, std::max(1, content_width - 2))) |
                color(theme().ui.text_muted));
        }

        // spawn_subagent 运行中任务:标题为空时退到 prompt 摘要。动态耗时
        // 由 transcript 内的运行中工具块统一展示,避免侧栏重复读秒。
        for (const auto& task : subagents) {
            const std::string label =
                task.title.empty() ? task.prompt : task.title;
            const int label_width = std::max(1, content_width - 4);
            bottom_rows.push_back(hbox({
                text("  "),
                text("\xE2\x97\x8F ") | color(theme().semantic.success),
                text(truncate_end(label, label_width)) |
                    color(theme().ui.text_muted) | flex,
            }));
        }
        bottom_rows.push_back(text(""));
    }
    bottom_rows.push_back(paragraph(version_str) | color(theme().ui.text_muted) | dim);
    if (!state.update_notice.empty()) {
        bottom_rows.push_back(paragraph(state.update_notice) |
                              color(theme().semantic.warning));
    }
    if (!state.status_line.empty()) {
        bottom_rows.push_back(paragraph(state.status_line) |
                              color(status_line_color(state.status_line)));
    }
    if (!cwd_display.empty()) {
        bottom_rows.push_back(paragraph(cwd_display) | color(theme().ui.accent_alt) | dim);
    }

    const bool is_light = theme().name == "light";
    const Color sidebar_background =
        is_light ? Color::RGB(240, 240, 242) : Color::RGB(18, 18, 20);
    if (!expanded) {
        content_box = Box{1, 0, 1, 0};
        viewport_box = Box{1, 0, 1, 0};
        scrollbar_box = Box{1, 0, 1, 0};
        Element sidebar = hbox({
            text(" "),
            vbox({
                vbox(std::move(top_rows)),
                filler(),
                vbox(std::move(bottom_rows)),
            }) | flex,
            text(" "),
        }) | size(WIDTH, EQUAL, sidebar_width) |
           bgcolor(sidebar_background);
        return non_selectable(std::move(sidebar));
    }

    top_rows.insert(
        top_rows.end(),
        std::make_move_iterator(bottom_rows.begin()),
        std::make_move_iterator(bottom_rows.end()));

    const int previous_content_rows =
        content_box.IsEmpty() ? 0 : content_box.y_max - content_box.y_min + 1;
    const int previous_viewport_rows =
        viewport_box.IsEmpty()
            ? 0
            : viewport_box.y_max - viewport_box.y_min + 1;
    state.sidebar_scroll_top_row = clamp_vertical_scroll_top_row(
        state.sidebar_scroll_top_row,
        previous_content_rows,
        previous_viewport_rows);
    const int frame_focus_y = vertical_frame_focus_y_for_scroll_top(
        state.sidebar_scroll_top_row, previous_viewport_rows);

    Element document =
        vbox(std::move(top_rows)) |
        reflect_unclipped(content_box) |
        focusPosition(0, frame_focus_y);
    Element scrolling_document =
        thick_vscroll_bar(
            std::move(document),
            kExpandedScrollbarWidth,
            scrollbar_box) |
        yframe |
        flex;
    Element sidebar = hbox({
        text(" "),
        std::move(scrolling_document),
    }) | size(WIDTH, EQUAL, sidebar_width) |
       reflect(viewport_box) |
       bgcolor(sidebar_background);
    return non_selectable(std::move(sidebar));
}


} // namespace acecode::tui
