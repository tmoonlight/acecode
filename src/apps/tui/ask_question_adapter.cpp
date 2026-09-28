#include "ask_question_adapter.hpp"

#include <algorithm>

namespace acecode::tui {

void AskQuestionFrame::reset_for_render() {
    overlay_box = ftxui::Box{0, -1, 0, -1};
    scrollbar_box = ftxui::Box{0, -1, 0, -1};
    row_boxes.assign(static_cast<std::size_t>(std::max(0, layout.visible_rows)),
                     ftxui::Box{0, -1, 0, -1});
}

bool ask_question_box_contains(const ftxui::Box& box, int x, int y) {
    return box.x_min <= box.x_max && box.y_min <= box.y_max &&
           box.Contain(x, y);
}

AskQuestionHit hit_test_ask_question_frame(const AskQuestionFrame& frame,
                                            int x,
                                            int y) {
    if (frame.layout.total_rows > frame.layout.visible_rows &&
        ask_question_box_contains(frame.scrollbar_box, x, y)) {
        return {AskQuestionHitKind::Scrollbar, -1, -1};
    }
    for (std::size_t index = 0; index < frame.row_boxes.size(); ++index) {
        if (!ask_question_box_contains(frame.row_boxes[index], x, y)) continue;
        const int row = frame.layout.scroll_offset + static_cast<int>(index);
        if (row < 0 || row >= static_cast<int>(frame.layout.rows.size())) {
            return {};
        }
        const auto& layout_row = frame.layout.rows[static_cast<std::size_t>(row)];
        switch (layout_row.kind) {
            case AskQuestionLayoutKind::Option:
                return {AskQuestionHitKind::Option, layout_row.question_index,
                        layout_row.option_index};
            case AskQuestionLayoutKind::Custom:
                return {AskQuestionHitKind::Custom, layout_row.question_index,
                        layout_row.option_index};
            case AskQuestionLayoutKind::Summary:
            case AskQuestionLayoutKind::SummaryAnswer:
                return {AskQuestionHitKind::SummaryQuestion,
                        layout_row.question_index, -1};
            default:
                return {};
        }
    }
    return {};
}

} // namespace acecode::tui
