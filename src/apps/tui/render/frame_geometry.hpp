#pragma once
#include "tui/ask_question_adapter.hpp"
#include "tui/tui_helpers.hpp"
#include "tui/markdown/markdown_formatter.hpp"
#include <ftxui/screen/box.hpp>
#include <vector>

namespace acecode::tui {
// One owner for reflected hit regions. Rendering and input share this stable
// object; it cannot be copied while DOM reflect nodes retain member addresses.
struct FrameGeometry {
    FrameGeometry() = default;
    FrameGeometry(const FrameGeometry&) = delete;
    FrameGeometry& operator=(const FrameGeometry&) = delete;
    ftxui::Box scrollbar_box;
    AskQuestionFrame ask_question_frame;
    ftxui::Box sidebar_content_box;
    ftxui::Box sidebar_viewport_box;
    ftxui::Box sidebar_scrollbar_box;
    InputTextHitLayout input_hit_layout;
    std::vector<ftxui::Box> message_boxes;
    std::vector<ftxui::Box> path_reference_boxes;
    markdown::MarkdownLinkRegionCollector chat_link_regions;
};
} // namespace acecode::tui
