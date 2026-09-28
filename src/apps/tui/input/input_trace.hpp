#pragma once
#include "acecode_tui_input_trace_config.hpp"

#if ACECODE_TUI_INPUT_TRACE
#include "tui/chat_scroll.hpp"
#include "tui/drag_scroll.hpp"
#include <ftxui/component/event.hpp>
#include <ftxui/screen/box.hpp>
#include <string>

namespace acecode::tui::input {
std::string box_for_log(const ftxui::Box& box);
std::string event_for_log(const ftxui::Event& event);
std::string drag_phase_for_log(acecode::drag_scroll::Phase phase);
std::string scrollbar_geometry_for_log(const ChatScrollbarThumbGeometry& geometry);
}
#define ACECODE_INPUT_TRACE(...) do { __VA_ARGS__ } while (false)
#else
#define ACECODE_INPUT_TRACE(...) do {} while (false)
#endif
