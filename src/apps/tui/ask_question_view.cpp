#include "ask_question_view.hpp"

#include <utility>

namespace acecode::tui {

ftxui::Element compose_ask_question_message_area(ftxui::Element message_view,
                                                 ftxui::Element ask_panel,
                                                 bool ask_active) {
    if (!ask_active) return message_view;
    // The filler above the panel pins it to the bottom of the shared chat
    // viewport: the panel keeps its own height, and everything below (prompt and
    // status bar) stays outside the overlay's area.
    return ftxui::dbox({std::move(message_view),
                        ftxui::vbox({ftxui::filler(), std::move(ask_panel)})});
}

} // namespace acecode::tui
