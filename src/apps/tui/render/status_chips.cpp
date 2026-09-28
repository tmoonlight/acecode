#include "tui/render/status_chips.hpp"
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

using ftxui::Element;
using ftxui::Elements;
using ftxui::Color;
using ftxui::text;
using ftxui::hbox;
using ftxui::vbox;
using ftxui::emptyElement;
using ftxui::size;
using ftxui::dim;
using ftxui::bold;
using ftxui::color;
using ftxui::bgcolor;
using ftxui::Utf8ToGlyphs;

#include "tui/render/text_cells.hpp"
#include "tui/render/status_chips.hpp"
#include "tui/model/mcp_sidebar_model.hpp"

namespace acecode::tui {
Color mcp_sidebar_state_color(const std::string& state) {
    if (state == "connected") return theme().semantic.success;
    if (state == "starting") return Color::White;
    if (state == "failed" || state == "timed_out") return theme().semantic.error;
    if (state == "cancelled") return theme().semantic.warning;
    return theme().ui.text_secondary;
}

Element render_white_shimmer_text(const std::string& label,
                                  int anim_tick,
                                  bool with_dots) {
    std::vector<std::string> glyphs = Utf8ToGlyphs(label);
    const int total = static_cast<int>(glyphs.size());
    const int wave_pos = std::max(0, anim_tick) % (total > 0 ? total + 2 : 8);

    Elements parts;
    for (int i = 0; i < total; ++i) {
        int dist = i - wave_pos;
        if (dist < 0) dist = -dist;
        Color c;
        if (dist == 0) {
            c = Color::White;
        } else if (dist == 1) {
            c = Color::GrayLight;
        } else if (dist == 2) {
            c = Color::GrayDark;
        } else {
            c = theme().ui.text_dim;
        }
        parts.push_back(text(glyphs[static_cast<std::size_t>(i)]) | color(c));
    }

    if (with_dots) {
        const int dot_count = (std::max(0, anim_tick) % 3) + 1;
        for (int i = 0; i < 3; ++i) {
            parts.push_back(
                text(".") |
                color(i < dot_count ? Color::White : theme().ui.text_dim));
        }
    }

    return hbox(std::move(parts));
}

Color token_progress_color(int percent) {
    const auto& s = theme().semantic;
    if (percent <= 0) return theme().ui.text_dim;
    if (percent > 90) return s.error;
    if (percent >= 60) return s.warning;
    return s.success;
}

std::atomic<int> g_model_load_percent{-1};

Color model_load_color(int percent) {
    const auto& s = theme().semantic;
    if (percent < 0) return theme().ui.text_dim;
    if (percent > 90) return s.error;
    if (percent >= 70) return s.warning;
    return s.success;
}

Element render_model_load_chip() {
    const int percent = g_model_load_percent.load();
    if (percent < 0) return text("");
    const Color c = model_load_color(percent);
    return hbox({
        text("\xE2\x96\x81\xE2\x96\x83\xE2\x96\x85\xE2\x96\x87") | color(c),
        text(" " + std::to_string(percent) + "%  ") | color(c),
    });
}

Color status_line_color(const std::string& status_line) {
    return status_line.find("(deleted)") != std::string::npos
        ? theme().semantic.error
        : theme().ui.text_primary;
}

// The only implementation of the status chips: main.cpp used to carry
// file-static twins of these helpers until P0-09 deleted those copies.
Element render_cache_hit_chip(const TuiState& state) {
    const std::string label =
        TokenTracker::format_cache_status_for(state.cache_hit_percent);
    if (label.empty()) {
        return text("");
    }
    return text(label + "  ") | dim | color(theme().ui.text_dim);
}

Element render_token_usage_chip(const TuiState& state) {
    if (state.token_status.empty()) {
        return text("");
    }

    constexpr int kBarCells = 10;
    constexpr const char* kFilled = "\xE2\x96\x88";
    constexpr const char* kEmpty = "\xE2\x96\x91";

    const int percent = std::clamp(state.token_percent, 0, 100);
    const int filled = percent <= 0 ? 0 : std::clamp((percent + 9) / 10, 1, kBarCells);
    const int empty = kBarCells - filled;
    const Color progress_color = token_progress_color(percent);

    return hbox({
        text("  " + state.token_status + " ") | dim | color(theme().ui.accent_alt),
        text("[") | dim | color(theme().ui.text_dim),
        text(repeat_utf8_glyph(kFilled, filled)) | color(progress_color),
        text(repeat_utf8_glyph(kEmpty, empty)) | dim | color(theme().ui.text_dim),
        text("] ") | dim | color(theme().ui.text_dim),
        text(std::to_string(percent) + "%  ") | dim | color(progress_color),
        render_cache_hit_chip(state),
    });
}

Element queued_badge() {
    return text(" QUEUED ") | bold | color(theme().ui.text_primary) |
           bgcolor(theme().ui.queued_bg);
}

Element render_pending_queue_block(const TuiState& state, int available_width) {
    if (state.pending_queue.empty()) {
        return emptyElement();
    }

    constexpr std::size_t kMaxVisibleQueuedPrompts = 3;
    constexpr int kBadgeCells = 8;
    const int prompt_width = std::max(10, available_width - kBadgeCells - 5);
    const std::size_t visible =
        std::min(kMaxVisibleQueuedPrompts, state.pending_queue.size());

    Elements rows;
    const std::size_t hidden =
        state.pending_queue.size() > visible
            ? state.pending_queue.size() - visible
            : 0;
    if (hidden > 0) {
        rows.push_back(
            text("  +" + std::to_string(hidden) + " more queued") |
            readable_secondary());
    }

    const std::size_t start = state.pending_queue.size() - visible;
    for (std::size_t i = start; i < state.pending_queue.size(); ++i) {
        const std::string preview = collapse_sidebar_title_whitespace(
            state.pending_queue[i]);
        rows.push_back(hbox({
            text(" "),
            queued_badge(),
            text(" "),
            text(truncate_cells_middle_ascii(preview, prompt_width)) |
                color(theme().ui.text_primary),
        }));
    }

    return vbox(std::move(rows));
}

Element render_pending_attachment_block(const TuiState& state, int available_width) {
    if (state.pending_attachments.empty()) {
        return emptyElement();
    }

    Elements rows;
    const int label_width = std::max(12, available_width - 18);
    const bool attachment_focus = has_pending_attachment_focus(
        state.pending_attachment_focus,
        state.pending_attachments.size());
    for (std::size_t i = 0; i < state.pending_attachments.size(); ++i) {
        const auto& attachment = state.pending_attachments[i];
        const bool focused = attachment_focus &&
            state.pending_attachment_focus == static_cast<int>(i);
        const std::string kind = attachment.value("kind", std::string{"file"});
        const std::string name = attachment.value("name", std::string{"attachment"});
        const std::string prefix = kind == "image" ? " image " : " file ";
        Element row = hbox({
            text(focused ? ">" : " "),
            text(prefix) | bold |
                color(focused ? theme().ui.selection_fg : theme().ui.badge_fg) |
                bgcolor(focused ? theme().ui.selection_bg : theme().ui.badge_bg),
            text(" "),
            text(truncate_cells_middle_ascii(name, label_width)) |
                color(focused ? theme().ui.selection_fg : theme().ui.text_primary),
        });
        if (focused) {
            row = row | bgcolor(theme().ui.selection_bg);
        }
        rows.push_back(std::move(row));
    }
    const std::string hint = attachment_focus
        ? "  Up/Down: select  Delete/Backspace: remove  Esc/Alt+A: input"
        : "  Alt+A: select attachments";
    rows.push_back(
        text(truncate_cells_middle_ascii(hint, std::max(12, available_width - 2))) |
        readable_secondary());
    return vbox(std::move(rows));
}


} // namespace acecode::tui
