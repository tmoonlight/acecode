#include "input_trace.hpp"
#if ACECODE_TUI_INPUT_TRACE
namespace acecode::tui {
std::string box_for_log(const ftxui::Box& box) {
    return "[" + std::to_string(box.x_min) + "," +
           std::to_string(box.y_min) + "]-[" +
           std::to_string(box.x_max) + "," +
           std::to_string(box.y_max) + "]";
}

std::string event_for_log(const ftxui::Event& event) {
    if (event.is_character()) {
        return "Event::Character(bytes=" +
               std::to_string(event.character().size()) + ")";
    }
    return event.DebugString();
}

std::string drag_phase_for_log(acecode::drag_scroll::Phase phase) {
    switch (phase) {
    case acecode::drag_scroll::Phase::Idle:
        return "Idle";
    case acecode::drag_scroll::Phase::Dragging:
        return "Dragging";
    case acecode::drag_scroll::Phase::ScrollingUp:
        return "ScrollingUp";
    case acecode::drag_scroll::Phase::ScrollingDown:
        return "ScrollingDown";
    }
    return "?";
}

std::string scrollbar_geometry_for_log(
    const acecode::tui::ChatScrollbarThumbGeometry& geometry) {
    return "{max_top=" + std::to_string(geometry.max_top_row) +
           " range2x=" + std::to_string(geometry.scroll_range_2x) +
           " thumb_size2x=" + std::to_string(geometry.thumb_size_2x) +
           " thumb_top2x=" + std::to_string(geometry.thumb_top_2x) + "}";
}
} // namespace acecode::tui
#endif
