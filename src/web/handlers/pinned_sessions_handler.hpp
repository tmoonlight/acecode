#pragma once

// Compatibility facade for Web/Desktop callers. Persistence is owned by the
// session domain so model-facing thread tools and Web use one file contract.

#include "../../session/session_pin_store.hpp"

namespace acecode::web {

using session_pins::PinnedSessionOrderItem;
using session_pins::PinnedSessionOrderState;
using session_pins::PinnedSessionsState;
using session_pins::normalize_pinned_session_ids;
using session_pins::normalize_pinned_session_order_items;
using session_pins::pin_session_id;
using session_pins::prune_pinned_session_ids;
using session_pins::prune_pinned_session_order_items;
using session_pins::read_pinned_session_order_state;
using session_pins::read_pinned_sessions_state;
using session_pins::unpin_session_id;
using session_pins::write_pinned_session_order_state;
using session_pins::write_pinned_sessions_state;

} // namespace acecode::web
