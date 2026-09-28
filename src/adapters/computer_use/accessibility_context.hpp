#pragma once

#include <nlohmann/json.hpp>

namespace acecode::computer_use {

inline void append_accessibility_focus_actions(nlohmann::json& actions, bool enabled, bool offscreen, bool focusable) {
    if (enabled && !offscreen && focusable) {
        actions.push_back("focus");
        actions.push_back("raise");
    }
}

// A focused readable control is authoritative. Otherwise a visible document
// provides context even when toolbar buttons or other non-text controls own focus.
inline int accessibility_document_priority(bool password, bool offscreen, bool focused, bool document) {
    if (password || offscreen) return 0;
    if (focused) return 2;
    return document ? 1 : 0;
}

} // namespace acecode::computer_use
