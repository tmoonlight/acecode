#include "tui/composer/input_wrap_view.hpp"
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
bool is_space_glyph(const std::string& glyph) {
    return glyph == " " || glyph == "\t";
}

bool is_narrow_glyph(const std::string& glyph) {
    return string_width(glyph) == 1;
}

bool is_opening_cjk_punctuation(const std::string& glyph) {
    static constexpr std::array<std::string_view, 8> kOpening = {
        "\xEF\xBC\x88",  // （
        "\xE3\x80\x8A",  // 《
        "\xE3\x80\x8C",  // 「
        "\xE3\x80\x90",  // 【
        "\xE2\x80\x98",  // '
        "\xE2\x80\x9C",  // "
        "\xE3\x80\x88",  // 〈
        "\xE3\x80\x8E",  // 『
    };
    for (const auto& candidate : kOpening) {
        if (glyph == candidate) {
            return true;
        }
    }
    return false;
}

bool is_closing_cjk_punctuation(const std::string& glyph) {
    static constexpr std::array<std::string_view, 15> kClosing = {
        "\xEF\xBC\x8C",  // ，
        "\xE3\x80\x82",  // 。
        "\xEF\xBC\x81",  // ！
        "\xEF\xBC\x9F",  // ？
        "\xEF\xBC\x9B",  // ；
        "\xEF\xBC\x9A",  // ：
        "\xE3\x80\x81",  // 、
        "\xEF\xBC\x89",  // ）
        "\xE3\x80\x8B",  // 》
        "\xE3\x80\x8D",  // 」
        "\xE3\x80\x91",  // 】
        "\xE2\x80\x99",  // '
        "\xE2\x80\x9D",  // "
        "\xE3\x80\x89",  // 〉
        "\xE3\x80\x8F",  // 』
    };
    for (const auto& candidate : kClosing) {
        if (glyph == candidate) {
            return true;
        }
    }
    return false;
}

void flush_ascii_run(std::string* ascii_run,
                     std::string* pending_prefix,
                     std::vector<std::string>* output) {
    if (ascii_run->empty()) {
        return;
    }

    std::string token = std::move(*ascii_run);
    ascii_run->clear();
    if (!pending_prefix->empty()) {
        token = std::move(*pending_prefix) + token;
        pending_prefix->clear();
    }
    output->push_back(std::move(token));
}

std::vector<std::string> tokenize_wrapped_input(const std::string& text) {
    std::vector<std::string> tokens;
    std::string ascii_run;
    std::string pending_prefix;

    for (const auto& glyph : Utf8ToGlyphs(text)) {
        if (glyph.empty()) {
            continue;
        }

        if (is_space_glyph(glyph)) {
            flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
            // An unresolved opening-punctuation run is logically after every
            // emitted token. Keep following whitespace with that run so token
            // concatenation cannot move the whitespace ahead of the opening
            // punctuation. Preserve the original glyph (including a tab).
            if (!pending_prefix.empty()) {
                pending_prefix += glyph;
            } else if (!tokens.empty()) {
                tokens.back() += glyph;
            } else {
                tokens.push_back(glyph);
            }
            continue;
        }

        if (is_opening_cjk_punctuation(glyph)) {
            flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
            pending_prefix += glyph;
            continue;
        }

        if (is_closing_cjk_punctuation(glyph)) {
            flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
            // pending_prefix comes after all emitted tokens. Resolve closing
            // punctuation there first; appending it to tokens.back() would
            // cross the pending opening run and visibly reverse text such as
            // full-width nested parentheses.
            if (!pending_prefix.empty()) {
                pending_prefix += glyph;
            } else if (!tokens.empty()) {
                tokens.back() += glyph;
            } else {
                tokens.push_back(glyph);
            }
            continue;
        }

        if (is_narrow_glyph(glyph)) {
            ascii_run += glyph;
            continue;
        }

        flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
        std::string token = glyph;
        if (!pending_prefix.empty()) {
            token = std::move(pending_prefix) + token;
            pending_prefix.clear();
        }
        tokens.push_back(std::move(token));
    }

    flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
    if (!pending_prefix.empty()) {
        if (!tokens.empty()) {
            tokens.back() += pending_prefix;
        } else {
            tokens.push_back(std::move(pending_prefix));
        }
    }

    return tokens;
}

Element render_wrapped_input_text(
    const std::string& input_value,
    size_t cursor_bytes,
    std::vector<InputTextHitRegion>* hit_regions,
    std::optional<size_t> selection_anchor) {
    cursor_bytes = clamp_utf8_boundary(input_value, cursor_bytes);
    const auto selection = text_selection_range(
        input_value, cursor_bytes, selection_anchor);

    std::string head = input_value.substr(0, cursor_bytes);
    std::string cursor_glyph;
    std::string tail;
    if (cursor_bytes < input_value.size()) {
        size_t next = cursor_bytes + 1;
        while (next < input_value.size() &&
               (static_cast<unsigned char>(input_value[next]) & 0xC0) == 0x80) {
            next++;
        }
        cursor_glyph = input_value.substr(cursor_bytes, next - cursor_bytes);
        tail = input_value.substr(next);
    }

    auto tokens_head = tokenize_wrapped_input(head);
    auto tokens_tail = tokenize_wrapped_input(tail);

    std::vector<std::pair<size_t, size_t>> head_ranges;
    head_ranges.reserve(tokens_head.size());
    size_t byte_offset = 0;
    for (const auto& token : tokens_head) {
        const size_t next_offset = byte_offset + token.size();
        head_ranges.emplace_back(byte_offset, next_offset);
        byte_offset = next_offset;
    }

    std::vector<std::pair<size_t, size_t>> tail_ranges;
    tail_ranges.reserve(tokens_tail.size());
    byte_offset = cursor_bytes + cursor_glyph.size();
    for (const auto& token : tokens_tail) {
        const size_t next_offset = byte_offset + token.size();
        tail_ranges.emplace_back(byte_offset, next_offset);
        byte_offset = next_offset;
    }

    if (hit_regions) {
        hit_regions->clear();
        hit_regions->reserve(
            (tokens_head.size() + tokens_tail.size()) * 3 + 1);
    }

    auto track = [&](Element element, size_t begin, size_t end) {
        if (!hit_regions) {
            return element;
        }
        hit_regions->push_back({Box{0, -1, 0, -1}, begin, end});
        return std::move(element) | reflect(hit_regions->back().box);
    };

    auto decorate_selection = [](Element element, bool selected) {
        if (!selected) {
            return element;
        }
        return std::move(element) |
            color(theme().ui.selection_fg) |
            bgcolor(theme().ui.selection_bg);
    };

    auto render_token = [&](const std::string& token,
                            size_t begin,
                            size_t end) {
        if (!selection.has_value() ||
            end <= selection->begin || begin >= selection->end) {
            return track(text(token), begin, end);
        }

        Elements fragments;
        const size_t selected_begin = std::max(begin, selection->begin);
        const size_t selected_end = std::min(end, selection->end);
        auto append_fragment = [&](size_t fragment_begin,
                                   size_t fragment_end,
                                   bool selected) {
            if (fragment_begin >= fragment_end) {
                return;
            }
            auto fragment = text(input_value.substr(
                fragment_begin, fragment_end - fragment_begin));
            fragments.push_back(track(
                decorate_selection(std::move(fragment), selected),
                fragment_begin,
                fragment_end));
        };
        append_fragment(begin, selected_begin, false);
        append_fragment(selected_begin, selected_end, true);
        append_fragment(selected_end, end, false);
        return hbox(std::move(fragments));
    };

    const bool cursor_glyph_selected =
        selection.has_value() && !cursor_glyph.empty() &&
        cursor_bytes < selection->end &&
        cursor_bytes + cursor_glyph.size() > selection->begin;
    auto cursor_element =
        text(cursor_glyph.empty() ? std::string(" ") : cursor_glyph) |
        focusCursorBlock;
    auto cursor_elem = track(
        decorate_selection(std::move(cursor_element), cursor_glyph_selected),
        cursor_bytes,
        cursor_bytes + cursor_glyph.size());

    if (tokens_head.empty() && tokens_tail.empty()) {
        return cursor_elem;
    }

    Elements parts;
    parts.reserve(tokens_head.size() + tokens_tail.size() + 1);

    for (size_t i = 0; i + 1 < tokens_head.size(); ++i) {
        parts.push_back(render_token(
            tokens_head[i],
            head_ranges[i].first,
            head_ranges[i].second));
    }

    Elements compound;
    if (!tokens_head.empty()) {
        compound.push_back(render_token(
            tokens_head.back(),
            head_ranges.back().first,
            head_ranges.back().second));
    }
    compound.push_back(cursor_elem);
    size_t tail_start = 0;
    if (!tokens_tail.empty()) {
        compound.push_back(render_token(
            tokens_tail[0],
            tail_ranges[0].first,
            tail_ranges[0].second));
        tail_start = 1;
    }
    parts.push_back(hbox(std::move(compound)));

    for (size_t i = tail_start; i < tokens_tail.size(); ++i) {
        parts.push_back(render_token(
            tokens_tail[i],
            tail_ranges[i].first,
            tail_ranges[i].second));
    }

    static const auto config = FlexboxConfig().SetGap(0, 0);
    return flexbox(std::move(parts), config);
}

Element render_empty_input_prompt(
    std::vector<InputTextHitRegion>* hit_regions) {
    Element row = hbox({
        text(" ") | focusCursorBlock,
        text("Type your prompt here...") | readable_secondary(),
    });
    if (hit_regions) {
        hit_regions->clear();
        hit_regions->reserve(1);
        hit_regions->push_back({Box{0, -1, 0, -1}, 0, 0});
        row = std::move(row) | reflect(hit_regions->back().box);
    }

    // The surrounding input uses vertical flex. Keep the reflected empty row
    // at one rendered line so blank space allocated below it is not clickable.
    return vbox({std::move(row)});
}

std::optional<size_t> input_cursor_from_point(
    const std::string& input_value,
    const Box& input_box,
    const std::vector<InputTextHitRegion>& hit_regions,
    int mouse_x,
    int mouse_y) {
    if (input_box.IsEmpty() || !input_box.Contain(mouse_x, mouse_y)) {
        return std::nullopt;
    }
    std::vector<const InputTextHitRegion*> row_regions;
    row_regions.reserve(hit_regions.size());
    for (const auto& region : hit_regions) {
        if (!region.box.IsEmpty() &&
            region.box.y_min <= mouse_y && mouse_y <= region.box.y_max &&
            region.byte_begin <= region.byte_end &&
            region.byte_end <= input_value.size()) {
            row_regions.push_back(&region);
        }
    }
    if (row_regions.empty()) {
        return std::nullopt;
    }

    if (input_value.empty()) {
        return size_t{0};
    }

    std::sort(
        row_regions.begin(), row_regions.end(),
        [](const InputTextHitRegion* lhs, const InputTextHitRegion* rhs) {
            if (lhs->box.x_min != rhs->box.x_min) {
                return lhs->box.x_min < rhs->box.x_min;
            }
            return lhs->byte_begin < rhs->byte_begin;
        });

    if (mouse_x < row_regions.front()->box.x_min) {
        return row_regions.front()->byte_begin;
    }

    for (const auto* region : row_regions) {
        if (mouse_x < region->box.x_min) {
            return region->byte_begin;
        }
        if (mouse_x > region->box.x_max) {
            continue;
        }

        int remaining_cells = mouse_x - region->box.x_min;
        size_t cursor = region->byte_begin;
        const std::string fragment = input_value.substr(
            region->byte_begin, region->byte_end - region->byte_begin);
        for (const auto& glyph : Utf8ToGlyphs(fragment)) {
            if (glyph.empty()) {
                continue;
            }
            if (remaining_cells <= 0) {
                break;
            }
            remaining_cells -= std::max(0, string_width(glyph));
            cursor += glyph.size();
        }
        return std::min(cursor, region->byte_end);
    }

    return row_regions.back()->byte_end;
}

std::optional<ShiftArrowDirection> shift_arrow_direction(
    const Event& event) {
    constexpr auto shift = terminal_modifier(TerminalKeyModifier::Shift);
    if (matches_terminal_key(event, TerminalKey::ArrowUp, shift)) {
        return ShiftArrowDirection::Up;
    }
    if (matches_terminal_key(event, TerminalKey::ArrowDown, shift)) {
        return ShiftArrowDirection::Down;
    }
    if (matches_terminal_key(event, TerminalKey::ArrowRight, shift)) {
        return ShiftArrowDirection::Right;
    }
    if (matches_terminal_key(event, TerminalKey::ArrowLeft, shift)) {
        return ShiftArrowDirection::Left;
    }
    return std::nullopt;
}

std::optional<size_t> input_cursor_vertical_target(
    const std::string& input_value,
    const Box& input_box,
    const std::vector<InputTextHitRegion>& hit_regions,
    size_t cursor_bytes,
    ShiftArrowDirection direction,
    std::optional<int>* goal_column) {
    if (direction != ShiftArrowDirection::Up &&
        direction != ShiftArrowDirection::Down) {
        return std::nullopt;
    }
    if (input_box.IsEmpty() || hit_regions.empty()) {
        return std::nullopt;
    }

    cursor_bytes = clamp_utf8_boundary(input_value, cursor_bytes);
    const InputTextHitRegion* cursor_region = nullptr;
    for (const auto& region : hit_regions) {
        if (!region.box.IsEmpty() && region.byte_begin == cursor_bytes) {
            cursor_region = &region;
            break;
        }
    }
    if (!cursor_region) {
        return std::nullopt;
    }

    std::vector<int> rows;
    rows.reserve(hit_regions.size());
    for (const auto& region : hit_regions) {
        if (!region.box.IsEmpty() &&
            region.byte_begin <= region.byte_end &&
            region.byte_end <= input_value.size()) {
            rows.push_back(region.box.y_min);
        }
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    const auto current_row = std::find(
        rows.begin(), rows.end(), cursor_region->box.y_min);
    if (current_row == rows.end()) {
        return std::nullopt;
    }
    auto target_row = current_row;
    if (direction == ShiftArrowDirection::Up) {
        if (target_row == rows.begin()) {
            return cursor_bytes;
        }
        --target_row;
    } else {
        ++target_row;
        if (target_row == rows.end()) {
            return cursor_bytes;
        }
    }

    int desired_column = cursor_region->box.x_min - input_box.x_min;
    if (goal_column) {
        if (!goal_column->has_value()) {
            *goal_column = desired_column;
        }
        desired_column = **goal_column;
    }
    const int mouse_x = std::clamp(
        input_box.x_min + desired_column,
        input_box.x_min,
        input_box.x_max);
    return input_cursor_from_point(
        input_value, input_box, hit_regions, mouse_x, *target_row);
}

// ask_session owns whether the custom answer editor is active.
InputPointerTarget input_pointer_target(const TuiState& state) {
    if (state.confirm_pending ||
        state.rewind_picker_active ||
        state.resume_picker_active ||
        state.model_picker_open ||
        state.mode_picker_open) {
        return InputPointerTarget::None;
    }
    if (state.ask_pending) {
        return state.ask_session && state.ask_session->snapshot().editing_custom
            ? InputPointerTarget::AskOther
            : InputPointerTarget::None;
    }
    return InputPointerTarget::Composer;
}

InputPointerPressResult resolve_input_pointer_press(
    const TuiState& state,
    const InputTextHitLayout& hit_layout,
    int mouse_x,
    int mouse_y) {
    const auto target = input_pointer_target(state);
    if (target == InputPointerTarget::None ||
        state.input_text != hit_layout.input_value) {
        return {};
    }

    const auto cursor = input_cursor_from_point(
        state.input_text,
        hit_layout.box,
        hit_layout.regions,
        mouse_x,
        mouse_y);
    if (!cursor.has_value()) {
        return {};
    }

    // Like grok-build's textarea mouse-down path, caret placement and drag
    // selection startup share the same press. FTXUI owns the selection anchor,
    // so the caller must leave this event unconsumed after applying the cursor.
    return InputPointerPressResult{
        true,
        false,
        target,
        *cursor,
    };
}


} // namespace acecode::tui
