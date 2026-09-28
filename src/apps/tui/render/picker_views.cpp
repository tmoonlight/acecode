#include "tui/render/picker_views.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_style.hpp"
#include <algorithm>
#include <utility>
#include <ftxui/screen/string.hpp>
using namespace ftxui;
#include "tui/picker_scroll.hpp"

namespace acecode::tui {
PickerViews render_picker_views(const TuiState& state) {
    // -- Prompt line --
    // Resume picker overlay above the prompt
    Element resume_picker_element = emptyElement();
    if (state.resume_picker_active && !state.resume_items.empty()) {
        Elements picker_rows;
        picker_rows.push_back(
            text(" Resume a session (Up/Down/PgUp/PgDn/Home/End to navigate, Enter to confirm, Esc to cancel, 1-9 jump):")
            | bold | color(tui::theme().ui.border));
        picker_rows.push_back(text(""));

        const int total = static_cast<int>(state.resume_items.size());
        const int visible = std::min(acecode::tui::kResumePickerVisibleRows, total);
        int offset = std::clamp(state.resume_view_offset, 0,
                                std::max(0, total - visible));
        const int items_above = offset;
        const int items_below = std::max(0, total - offset - visible);

        // Top overflow indicator (always reserves a row to keep height stable).
        if (items_above > 0) {
            picker_rows.push_back(
                text("  \xE2\x86\x91 " + std::to_string(items_above) + " more above")
                | dim | color(tui::theme().ui.text_muted));
        } else {
            picker_rows.push_back(text(""));
        }

        for (int i = offset; i < offset + visible; ++i) {
            bool selected = (i == state.resume_selected);
            auto row = text("  " + state.resume_items[i].display);
            if (selected) {
                row = row | bold | color(tui::theme().ui.selection_fg) | bgcolor(tui::theme().ui.selection_bg);
            } else {
                row = row | color(tui::theme().ui.text_muted);
            }
            picker_rows.push_back(row);
        }

        // Bottom overflow indicator (also reserves a row).
        if (items_below > 0) {
            picker_rows.push_back(
                text("  \xE2\x86\x93 " + std::to_string(items_below) + " more below")
                | dim | color(tui::theme().ui.text_muted));
        } else {
            picker_rows.push_back(text(""));
        }

        picker_rows.push_back(text(""));
        resume_picker_element = vbox(std::move(picker_rows)) | border | color(tui::theme().ui.border);
    }
    Element rewind_picker_element = emptyElement();
    if (state.rewind_picker_active && !state.rewind_items.empty()) {
        Elements picker_rows;
        if (state.rewind_mode_active) {
            const int selected =
                std::clamp(state.rewind_selected, 0,
                           static_cast<int>(state.rewind_items.size()) - 1);
            const auto& item = state.rewind_items[selected];
            picker_rows.push_back(
                text(" Rewind mode (Up/Down to select, Enter to confirm, Esc to go back, or type 1-9):")
                | bold | color(tui::theme().ui.border));
            picker_rows.push_back(text(" Target: " + item.preview) | color(tui::theme().ui.text_muted));
            picker_rows.push_back(text(" Code rewind only covers ACECode file_edit/file_write changes; manual edits, shell commands, MCP tools, git operations, and external side effects are not tracked.")
                                  | color(tui::theme().ui.accent));
            picker_rows.push_back(text(""));
            for (int i = 0; i < static_cast<int>(state.rewind_modes.size()); ++i) {
                bool selected_mode = (i == state.rewind_mode_selected);
                const auto& mode = state.rewind_modes[i];
                auto row = hbox({
                    text("  [" + std::to_string(i + 1) + "] " + mode.label + "  "),
                    text(mode.description) | color(tui::theme().ui.text_muted),
                });
                if (selected_mode) {
                    row = row | bold | color(tui::theme().ui.selection_fg) | bgcolor(tui::theme().ui.selection_bg);
                } else {
                    row = row | color(tui::theme().ui.text_muted);
                }
                picker_rows.push_back(row);
            }
        } else {
            const bool is_fork =
                state.rewind_picker_operation ==
                TuiState::RewindPickerOperation::Fork;
            picker_rows.push_back(
                text(std::string(is_fork
                    ? " Fork from a user turn"
                    : " Rewind to a user turn") +
                    " (Up/Down/PgUp/PgDn/Home/End to navigate, Enter to confirm, Esc to cancel, 1-9 jump):")
                | bold | color(tui::theme().ui.border));
            picker_rows.push_back(text(""));

            const int total = static_cast<int>(state.rewind_items.size());
            const int visible = std::min(acecode::tui::kRewindPickerVisibleRows, total);
            int offset = std::clamp(state.rewind_view_offset, 0,
                                    std::max(0, total - visible));
            const int items_above = offset;
            const int items_below = std::max(0, total - offset - visible);

            if (items_above > 0) {
                picker_rows.push_back(
                    text("  \xE2\x86\x91 " + std::to_string(items_above) + " more above")
                    | dim | color(tui::theme().ui.text_muted));
            } else {
                picker_rows.push_back(text(""));
            }

            for (int i = offset; i < offset + visible; ++i) {
                bool selected = (i == state.rewind_selected);
                auto row = text("  " + state.rewind_items[i].display);
                if (selected) {
                    row = row | bold | color(tui::theme().ui.selection_fg) | bgcolor(tui::theme().ui.selection_bg);
                } else {
                    row = row | color(tui::theme().ui.text_muted);
                }
                picker_rows.push_back(row);
            }

            if (items_below > 0) {
                picker_rows.push_back(
                    text("  \xE2\x86\x93 " + std::to_string(items_below) + " more below")
                    | dim | color(tui::theme().ui.text_muted));
            } else {
                picker_rows.push_back(text(""));
            }
        }
        picker_rows.push_back(text(""));
        rewind_picker_element = vbox(std::move(picker_rows)) | border | color(tui::theme().ui.border);
    }
    // /model picker overlay。同 resume_picker_element 的视觉风格(青边、
    // 滚动指示、选中行高亮)—— 单纯多一列前缀 "*" 标记当前 effective entry。
    Element model_picker_element = emptyElement();
    if (state.model_picker_open && !state.model_picker_options.empty()) {
        Elements picker_rows;
        picker_rows.push_back(
            text(" Select a model (Up/Down/PgUp/PgDn/Home/End to navigate, Enter to confirm, Esc to cancel):")
            | bold | color(tui::theme().ui.border));
        picker_rows.push_back(text(""));

        const int total = static_cast<int>(state.model_picker_options.size());
        // 复用 /resume 的视口高度常量 —— picker 行为口径一致,行少时
        // scroll_to_keep_visible 直接返回 0,不会留空行。
        const int visible = std::min(acecode::tui::kResumePickerVisibleRows, total);
        int offset = std::clamp(state.model_picker_view_offset, 0,
                                std::max(0, total - visible));
        const int items_above = offset;
        const int items_below = std::max(0, total - offset - visible);

        if (items_above > 0) {
            picker_rows.push_back(
                text("  \xE2\x86\x91 " + std::to_string(items_above) + " more above")
                | dim | color(tui::theme().ui.text_muted));
        } else {
            picker_rows.push_back(text(""));
        }

        for (int i = offset; i < offset + visible; ++i) {
            bool selected = (i == state.model_picker_selected);
            const auto& opt = state.model_picker_options[i];
            std::string marker = opt.is_current ? "* " : "  ";
            std::string body =
                "  " + marker + opt.name + "  (" + opt.provider + "/" + opt.model + ")";
            auto row = text(body);
            if (selected) {
                row = row | bold | color(tui::theme().ui.selection_fg) | bgcolor(tui::theme().ui.selection_bg);
            } else if (opt.is_current) {
                row = row | color(tui::theme().ui.accent);
            } else {
                row = row | color(tui::theme().ui.text_muted);
            }
            picker_rows.push_back(row);
        }

        if (items_below > 0) {
            picker_rows.push_back(
                text("  \xE2\x86\x93 " + std::to_string(items_below) + " more below")
                | dim | color(tui::theme().ui.text_muted));
        } else {
            picker_rows.push_back(text(""));
        }

        picker_rows.push_back(text(""));
        model_picker_element = vbox(std::move(picker_rows)) | border | color(tui::theme().ui.border);
    }

    Element mode_picker_element = emptyElement();
    if (state.mode_picker_open && !state.mode_picker_options.empty()) {
        Elements picker_rows;
        picker_rows.push_back(
            text(" Select a permission mode (Up/Down/Home/End to navigate, Enter to confirm, Esc to cancel):")
            | bold | color(tui::theme().ui.border));
        picker_rows.push_back(text(""));

        for (int i = 0; i < static_cast<int>(state.mode_picker_options.size()); ++i) {
            const bool selected = (i == state.mode_picker_selected);
            const auto& option = state.mode_picker_options[i];
            const std::string marker = option.is_current ? "* " : "  ";
            auto row = hbox({
                text("  [" + std::to_string(i + 1) + "] " + marker + option.name + "  "),
                text(option.description) | color(tui::theme().ui.text_muted),
            });
            if (selected) {
                row = row | bold | color(tui::theme().ui.selection_fg) |
                      bgcolor(tui::theme().ui.selection_bg);
            } else if (option.is_current) {
                row = row | color(tui::theme().ui.accent);
            } else {
                row = row | color(tui::theme().ui.text_muted);
            }
            picker_rows.push_back(row);
        }

        picker_rows.push_back(text(""));
        mode_picker_element =
            vbox(std::move(picker_rows)) | border | color(tui::theme().ui.border);
    }

    return {std::move(resume_picker_element), std::move(rewind_picker_element),
        std::move(model_picker_element), std::move(mode_picker_element)};
}

}
