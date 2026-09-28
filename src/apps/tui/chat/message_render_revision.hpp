#pragma once
#include "tui/tui_state.hpp"
#include <cstddef>
namespace acecode::tui {
// Content is accounted for by the separate render cache key, not this revision.
std::size_t message_render_revision(const TuiState::Message& message, bool transcript_expanded);
}
