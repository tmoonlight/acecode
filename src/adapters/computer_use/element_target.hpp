#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>

namespace acecode::computer_use::detail {

enum class ElementTargetError { none, unavailable, outside_window, obscured };

struct ElementClickTarget {
    POINT point{};
    ElementTargetError error = ElementTargetError::unavailable;
};

// Resolve a physical click from an observed UIA element. A hit on one of its
// descendants is valid; a sibling overlay in the same top-level window is not.
ElementClickTarget resolve_element_click_target(IUIAutomation* automation,
    IUIAutomationElement* element, HWND window, const RECT& element_rect, const RECT& window_rect);

} // namespace acecode::computer_use::detail
#endif
