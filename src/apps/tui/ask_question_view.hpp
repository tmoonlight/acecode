#pragma once

#include <ftxui/dom/elements.hpp>

namespace acecode::tui {

// AskUserQuestion is a visual overlay over the chat viewport. Keeping the
// overlay in a dbox prevents its content height from reducing the viewport whose
// height is used to calculate the overlay rows on the next frame.
//
// The panel is anchored to the BOTTOM of the chat viewport so it sits just above
// the prompt. It is never given a panel-wide focus: the terminal cursor is owned
// by the caret inside the inline editor, so there is no "input active" flag.
ftxui::Element compose_ask_question_message_area(ftxui::Element message_view,
                                                 ftxui::Element ask_panel,
                                                 bool ask_active);

} // namespace acecode::tui
