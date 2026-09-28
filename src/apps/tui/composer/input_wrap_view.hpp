#pragma once
#include "tui/tui_state.hpp"
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/box.hpp>
#include <optional>

namespace acecode::tui {
struct InputTextHitRegion {
    ftxui::Box box{0, -1, 0, -1};
    size_t byte_begin = 0;
    size_t byte_end = 0;
};

struct InputTextHitLayout {
    ftxui::Box box{0, -1, 0, -1};
    std::vector<InputTextHitRegion> regions;
    std::string input_value;
    void clear() {
        box = ftxui::Box{0, -1, 0, -1};
        regions.clear();
        input_value.clear();
    }
};

enum class InputPointerTarget {
    None,
    Composer,
    AskOther,
};

enum class ShiftArrowDirection {
    Up,
    Down,
    Left,
    Right,
};

struct InputPointerPressResult {
    bool cursor_placed = false;
    bool event_consumed = false;
    InputPointerTarget target = InputPointerTarget::None;
    size_t cursor_bytes = 0;
};

bool is_space_glyph(const std::string& glyph);
bool is_narrow_glyph(const std::string& glyph);
bool is_opening_cjk_punctuation(const std::string& glyph);
bool is_closing_cjk_punctuation(const std::string& glyph);
void flush_ascii_run(std::string* ascii_run, std::string* pending_prefix, std::vector<std::string>* output);
std::vector<std::string> tokenize_wrapped_input(const std::string& text);
ftxui::Element render_wrapped_input_text(
    const std::string& input_value,
    size_t cursor_bytes,
    std::vector<InputTextHitRegion>* hit_regions = nullptr,
    std::optional<size_t> selection_anchor = std::nullopt);
ftxui::Element render_empty_input_prompt(
    std::vector<InputTextHitRegion>* hit_regions = nullptr);
std::optional<size_t> input_cursor_from_point(
    const std::string& input_value,
    const ftxui::Box& input_box,
    const std::vector<InputTextHitRegion>& hit_regions,
    int mouse_x,
    int mouse_y);
std::optional<ShiftArrowDirection> shift_arrow_direction(
    const ftxui::Event& event);
std::optional<size_t> input_cursor_vertical_target(
    const std::string& input_value,
    const ftxui::Box& input_box,
    const std::vector<InputTextHitRegion>& hit_regions,
    size_t cursor_bytes,
    ShiftArrowDirection direction,
    std::optional<int>* goal_column);
InputPointerTarget input_pointer_target(const TuiState& state);
InputPointerPressResult resolve_input_pointer_press(
    const TuiState& state,
    const InputTextHitLayout& hit_layout,
    int mouse_x,
    int mouse_y);
} // namespace acecode::tui
