#pragma once

// Compatibility facade for Web/Desktop callers. Persistence is owned by the
// session domain so model-facing thread tools and Web use one file contract.

#include "session/session_pin_store.hpp"
#include <functional>

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

// The lookup is invoked only for supplied pins; an empty pin list does no IO.
inline std::vector<std::string> existing_pinned_session_ids(
    const std::vector<std::string>& candidates,
    const std::function<bool(const std::string&)>& exists) {
    std::vector<std::string> result;
    for (const auto& id : normalize_pinned_session_ids(candidates)) {
        if (id.size() > 128 ||
            id.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") != std::string::npos) continue;
        if (exists(id)) result.push_back(id);
    }
    return result;
}

} // namespace acecode::web
