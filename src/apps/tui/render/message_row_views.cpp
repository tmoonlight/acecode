#include "tui/render/message_row_views.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_style.hpp"
#include <algorithm>
#include <utility>
#include <ftxui/screen/string.hpp>
using ftxui::Element;
using ftxui::Color;
using ftxui::text;
using ftxui::paragraph;
using ftxui::hbox;
using ftxui::flex;
using ftxui::bold;
using ftxui::color;
using ftxui::bgcolor;
using ftxui::focus;
#include "tui/compact_notice_row.hpp"

namespace acecode::tui {
ftxui::Element render_user_message_row(const TuiState::Message& msg, bool focused_message) {
    // 用户消息整块加灰底高亮,把每一轮对话与助手/工具输出区分开。
    // flex 让内容撑满行宽,背景色铺满整块。
    const Color user_bg = (tui::theme().name == "light")
        ? Color::RGB(232, 232, 235)
        : Color::RGB(48, 48, 54);
    auto line = hbox({
        text(" > ") | bold | color(tui::theme().markdown.link),
        paragraph(msg.content) | color(tui::theme().ui.text_primary) | flex,
    }) | bgcolor(user_bg);
    if (focused_message) {
        line = line | focus;
    }
    return line;
}
ftxui::Element render_assistant_message_row(ftxui::Element md_content, bool focused_message) {
    // The active streaming message intentionally uses the same full
    // formatter as a completed message. L1 still skips unchanged
    // completed messages, while content growth naturally misses the
    // cache key and preserves full Markdown/XML semantics.
    auto line = hbox({
        text(" * ") | bold | color(tui::theme().semantic.success),
        md_content | flex,
    });
    if (focused_message) {
        line = line | focus;
    }
    return line;
}
ftxui::Element render_tool_text_row(ftxui::Element content, bool focused_message) {
    auto line = hbox({
        text("  \xE2\x94\x94 ") | color(theme().ui.text_dim),
        std::move(content) | flex,
    });
    if (focused_message) line = line | focus;
    return line;
}
ftxui::Element render_notice_message_row(const TuiState::Message& msg,
    bool transcript_expanded, bool focused_message) {
    if (msg.role == "compact_notice") {
        const bool row_expanded = tui::compact_notice_row_is_expanded(
            msg, transcript_expanded);
        Element content = row_expanded
            ? paragraph(msg.content) | color(tui::theme().ui.accent)
            : hbox({
                  text(tui::kCollapsedCompactNoticeLabel) | bold,
                  text("  (Ctrl+E to expand)") |
                      color(tui::theme().ui.text_dim),
              });
        auto line = hbox({
            text(" i ") | bold | color(tui::theme().ui.accent),
            std::move(content) | flex,
        });
        if (focused_message) {
            line = line | focus;
        }
        return line;
    } else if (msg.role == "system") {
        auto line = hbox({
            text(" i ") | bold | color(tui::theme().ui.accent),
            paragraph(msg.content) | color(tui::theme().ui.accent) | flex,
        });
        if (focused_message) {
            line = line | focus;
        }
        return line;
    } else if (msg.role == "turn_done") {
        // inline-thinking-heartbeat:回合收尾伪行 "● Done for Ns"。
        // 与推理行同款 "●" 前缀同列对齐,使用可读次级色表现推理行
        // 落定后的余烬。显示端专属 role,不进持久化/LLM context。
        auto line = hbox({
            text(" \xE2\x97\x8F ") | tui::readable_secondary(),
            paragraph(msg.content) | tui::readable_secondary() | flex,
        });
        if (focused_message) {
            line = line | focus;
        }
        return line;
    } else if (msg.role == "error") {
        auto line = hbox({
            text(" ! ") | bold | color(tui::theme().semantic.error),
            paragraph(msg.content) | color(tui::theme().semantic.error) | flex,
        });
        if (focused_message) {
            line = line | focus;
        }
        return line;
    }
    return nullptr;
}

}
