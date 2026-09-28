#include "tui/render/link_hover_tooltip.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_style.hpp"
#include <algorithm>
#include <utility>
#include <ftxui/screen/string.hpp>
using ftxui::Color;
using ftxui::text;
using ftxui::hbox;
using ftxui::vbox;
using ftxui::dbox;
using ftxui::emptyElement;
using ftxui::filler;
using ftxui::size;
using ftxui::WIDTH;
using ftxui::HEIGHT;
using ftxui::EQUAL;
using ftxui::color;
using ftxui::bgcolor;
using ftxui::border;
using ftxui::borderRounded;
#include "tui/render/text_cells.hpp"

namespace acecode::tui {
std::optional<LinkHoverPlacement> place_link_hover_tooltip(const TuiState& state,
    int terminal_width, int terminal_height) {
    // 边框至少需要 2 列/3 行;极窄终端直接跳过浮层,避免 dbox 的需求尺寸
    // 反向撑大主布局。
    if (terminal_width < 4 || terminal_height < 3) {
        return std::nullopt;
    }
    // 显示真实 URL(href 原文,防骗 —— 显示文本可能被 Markdown 伪装)。
    // 按 cell 而不是 UTF-8 字节截断;宽/高预算含 2 格 border 边框,确保
    // x + bubble_w <= dimx 恒成立,不会撑大 dbox 需求。
    const int max_url_cells = terminal_width - 2;
    const std::string url = tui::truncate_cells_middle_ascii(
        state.hover_link_href, max_url_cells);
    const int bubble_w = std::min(
        terminal_width, std::max(2, ftxui::string_width(url) + 2));
    const int bubble_h = 3;  // border top + text row + border bottom

    const int px = state.hover_link_x;
    const int py = state.hover_link_y;
    int x = px + 2;  // 指针右上方
    if (x + bubble_w > terminal_width) {
        x = px - bubble_w - 2;  // 右侧不够 → 指针左侧
    }
    x = std::clamp(x, 0, terminal_width - bubble_w);
    int y = py - bubble_h - 1;  // 指针上方
    if (y < 0) {
        y = py + 1;  // 上方不够 → 指针下方
    }
    y = std::min(y, std::max(0, terminal_height - bubble_h));

    return LinkHoverPlacement{url, x, y, bubble_w, bubble_h};
}
ftxui::Element render_link_hover_tooltip(const TuiState& state,
    int terminal_width, int terminal_height) {
    const auto placement = place_link_hover_tooltip(state, terminal_width, terminal_height);
    if (!placement) return emptyElement();
    const auto& url = placement->url;
    const int x = placement->x;
    const int y = placement->y;
    const bool is_light = acecode::tui::theme().name == "light";
    const Color bubble_bg =
        is_light ? Color::RGB(235, 238, 244) : Color::RGB(42, 46, 54);
    auto bubble =
        text(url) | color(acecode::tui::theme().ui.text_primary) |
        bgcolor(bubble_bg) |
        borderRounded | color(acecode::tui::theme().ui.border);
    return vbox({
        emptyElement() | size(HEIGHT, EQUAL, y),
        hbox({
            emptyElement() | size(WIDTH, EQUAL, x),
            bubble,
        }),
        filler(),
    });
}

}
