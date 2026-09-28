#include "macos_native.hpp"
#include "macos_keyboard.hpp"
#import <Carbon/Carbon.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace acecode::computer_use::macos {
namespace {
struct InputState {
    std::mutex mutex;
    bool revoked = false;
    std::set<CGKeyCode> keys;
    std::set<CGMouseButton> buttons;
    CGPoint point{};
};
InputState& input() { static InputState state; return state; }
void release_pressed(bool revoke) {
    auto& state = input();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.revoked = state.revoked || revoke;
    for (const auto key : state.keys) {
        Ref<CGEventRef> event(CGEventCreateKeyboardEvent(nullptr, key, false));
        if (event) { CGEventSetFlags(event.get(), 0); CGEventPost(kCGHIDEventTap, event.get()); }
    }
    for (const auto button : state.buttons) {
        const auto type = button == kCGMouseButtonLeft ? kCGEventLeftMouseUp : button == kCGMouseButtonRight ? kCGEventRightMouseUp : kCGEventOtherMouseUp;
        Ref<CGEventRef> event(CGEventCreateMouseEvent(nullptr, type, state.point, button));
        if (event) CGEventPost(kCGHIDEventTap, event.get());
    }
    state.keys.clear(); state.buttons.clear();
}
void post(CGEventRef event, CGKeyCode key = UINT16_MAX, int button = -1, bool down = false) {
    if (!event) throw Error("input_failed", "Could not create a native input event.");
    auto& state = input();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.revoked) throw Error("control_revoked", "Computer Use has been revoked.");
    if (key != UINT16_MAX) { if (down) state.keys.insert(key); else state.keys.erase(key); }
    if (button >= 0) {
        state.point = CGEventGetLocation(event);
        if (down) state.buttons.insert(static_cast<CGMouseButton>(button)); else state.buttons.erase(static_cast<CGMouseButton>(button));
    }
    CGEventPost(kCGHIDEventTap, event);
}
void focus_check(const Observation& observation, bool keyboard) {
    require_interactive_session();
    if (NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier != observation.foreground ||
        observation.foreground != observation.surfaces.front().window.pid)
        throw Error("focus_changed", "The observed application lost foreground. Activate and observe again.");
    AX app(AXUIElementCreateApplication(observation.foreground));
    auto focused_window = ax_attribute(app.get(), kAXFocusedWindowAttribute);
    const auto belongs = [&](AXUIElementRef element) {
        return element && std::any_of(observation.surfaces.begin(), observation.surfaces.end(),
            [&](const Surface& surface) { return ax_descendant(element, surface.window.root.get()); });
    };
    if (!focused_window || !belongs(focused_window.get()))
        throw Error("focus_changed", "Another window of this application has focus. Activate and observe the target again.");
    if (keyboard) {
        auto focus = ax_attribute(app.get(), kAXFocusedUIElementAttribute);
        if (!focus || !observation.focus || !CFEqual(focus.get(), observation.focus.get()) || !belongs(focus.get()))
            throw Error("focus_changed", "Keyboard focus changed or is unavailable. Observe again.");
    }
}
double number(const json& request, const char* key) {
    const auto value = request.find(key);
    if (value == request.end() || !value->is_number() || !std::isfinite(value->get<double>()))
        throw Error("invalid_argument", std::string(key) + " must be a finite number.");
    return value->get<double>();
}
std::size_t selected_surface(const json& request, const Observation& observation) {
    if (request.contains("screenshot_id")) {
        if (!request["screenshot_id"].is_string()) throw Error("invalid_screenshot", "screenshot_id must be a string.");
        const auto id = request["screenshot_id"].get<std::string>();
        for (std::size_t i = 0; i < observation.surfaces.size(); ++i)
            if (!id.empty() && observation.surfaces[i].screenshot_id == id) return i;
        throw Error("stale_screenshot", "Screenshot does not belong to this observation.");
    }
    if (observation.surfaces.size() != 1 || observation.surfaces[0].screenshot_id.empty())
        throw Error("screenshot_required", "Specify the exact screenshot_id for coordinate actions.");
    return 0;
}
CGPoint coordinate(const json& request, const char* xkey, const char* ykey, const Surface& surface) {
    if (surface.screenshot_id.empty()) throw Error("screenshot_required", "Capture the target surface before coordinate input.");
    const auto value = image_to_screen(number(request, xkey), number(request, ykey), surface.width, surface.height, screen_rect(surface.window.rect));
    if (!value) throw Error("invalid_coordinate", "Coordinates are outside the returned screenshot.");
    return CGPointMake(value->x, value->y);
}
Element& selected_element(const json& request, Observation& observation) {
    if (!request.contains("element_index") || !request["element_index"].is_number_integer())
        throw Error("invalid_element", "Use an element_index from this observation.");
    const auto index = request["element_index"].get<std::int64_t>();
    if (index < 0 || static_cast<std::size_t>(index) >= observation.elements.size()) throw Error("invalid_element", "Unknown element index.");
    auto& element = observation.elements[index];
    if (!same_rect(ax_bounds(element.ax.get()), element.rect) || !bool_attribute(element.ax.get(), kAXEnabledAttribute, true))
        throw Error("stale_element", "The control moved or became unavailable. Observe again.");
    if (request.contains("screenshot_id") && request["screenshot_id"] != observation.surfaces[element.surface].screenshot_id)
        throw Error("stale_screenshot", "Element and screenshot refer to different surfaces.");
    return element;
}
void mouse(CGEventType type, CGPoint point, CGMouseButton button, bool down, int count = 1) {
    Ref<CGEventRef> event(CGEventCreateMouseEvent(nullptr, type, point, button));
    if (event) CGEventSetIntegerValueField(event.get(), kCGMouseEventClickState, count);
    post(event.get(), UINT16_MAX, button, down);
}
}

KeyChord parse_key_chord(const std::string& value) {
    if (value.empty() || value.size() > 160) throw Error("invalid_key", "A key or chord is required.");
    static const std::map<std::string, std::pair<CGKeyCode, CGEventFlags>> modifiers{
        {"cmd", {kVK_Command, kCGEventFlagMaskCommand}}, {"command", {kVK_Command, kCGEventFlagMaskCommand}},
        {"meta", {kVK_Command, kCGEventFlagMaskCommand}}, {"shift", {kVK_Shift, kCGEventFlagMaskShift}},
        {"ctrl", {kVK_Control, kCGEventFlagMaskControl}}, {"control", {kVK_Control, kCGEventFlagMaskControl}},
        {"alt", {kVK_Option, kCGEventFlagMaskAlternate}}, {"option", {kVK_Option, kCGEventFlagMaskAlternate}}};
    static const std::map<std::string, CGKeyCode> named{
        {"enter", kVK_Return}, {"return", kVK_Return}, {"tab", kVK_Tab}, {"space", kVK_Space},
        {"escape", kVK_Escape}, {"esc", kVK_Escape}, {"backspace", kVK_Delete}, {"delete", kVK_ForwardDelete},
        {"left", kVK_LeftArrow}, {"arrowleft", kVK_LeftArrow}, {"right", kVK_RightArrow}, {"arrowright", kVK_RightArrow},
        {"up", kVK_UpArrow}, {"arrowup", kVK_UpArrow}, {"down", kVK_DownArrow}, {"arrowdown", kVK_DownArrow},
        {"home", kVK_Home}, {"end", kVK_End}, {"pageup", kVK_PageUp}, {"pagedown", kVK_PageDown},
        {"f1", kVK_F1}, {"f2", kVK_F2}, {"f3", kVK_F3}, {"f4", kVK_F4}, {"f5", kVK_F5}, {"f6", kVK_F6},
        {"f7", kVK_F7}, {"f8", kVK_F8}, {"f9", kVK_F9}, {"f10", kVK_F10}, {"f11", kVK_F11}, {"f12", kVK_F12},
        {"f13", kVK_F13}, {"f14", kVK_F14}, {"f15", kVK_F15}, {"f16", kVK_F16}, {"f17", kVK_F17}, {"f18", kVK_F18},
        {"f19", kVK_F19}, {"f20", kVK_F20}, {"numpadenter", kVK_ANSI_KeypadEnter}, {"numpad0", kVK_ANSI_Keypad0},
        {"numpad1", kVK_ANSI_Keypad1}, {"numpad2", kVK_ANSI_Keypad2}, {"numpad3", kVK_ANSI_Keypad3},
        {"numpad4", kVK_ANSI_Keypad4}, {"numpad5", kVK_ANSI_Keypad5}, {"numpad6", kVK_ANSI_Keypad6},
        {"numpad7", kVK_ANSI_Keypad7}, {"numpad8", kVK_ANSI_Keypad8}, {"numpad9", kVK_ANSI_Keypad9},
        {"numpadadd", kVK_ANSI_KeypadPlus}, {"numpadsubtract", kVK_ANSI_KeypadMinus}, {"numpadmultiply", kVK_ANSI_KeypadMultiply},
        {"numpaddivide", kVK_ANSI_KeypadDivide}, {"numpaddecimal", kVK_ANSI_KeypadDecimal},
        {"plus", kVK_ANSI_Equal}, {"minus", kVK_ANSI_Minus}};
    KeyChord chord;
    bool has_key = false;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find('+', begin);
        auto token = value.substr(begin, end == std::string::npos ? end : end - begin);
        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (token.empty()) throw Error("invalid_key", "Empty chord component. Use the named Plus key.");
        if (const auto it = modifiers.find(token); it != modifiers.end()) {
            if (has_key || (chord.flags & it->second.second)) throw Error("invalid_key", "Duplicate or misplaced chord modifier.");
            chord.modifiers.push_back(it->second.first); chord.flags |= it->second.second;
        } else {
            if (has_key) throw Error("invalid_key", "A chord must have exactly one non-modifier key.");
            if (const auto key = named.find(token); key != named.end()) chord.key = key->second;
            else {
                NSString* character = ns(token);
                if (character.length != 1) throw Error("invalid_key", "Unknown key name.");
                Ref<TISInputSourceRef> layout(TISCopyCurrentKeyboardLayoutInputSource());
                auto data = layout ? static_cast<CFDataRef>(TISGetInputSourceProperty(layout.get(), kTISPropertyUnicodeKeyLayoutData)) : nullptr;
                if (!data) throw Error("keyboard_layout_unavailable", "No keyboard layout is available for this chord.");
                bool found = false;
                for (UInt16 key = 0; key < 128 && !found; ++key) {
                    UInt32 dead = 0; UniChar output[4]; UniCharCount count = 0;
                    if (UCKeyTranslate(reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data)), key, kUCKeyActionDown,
                        0, LMGetKbdType(), kUCKeyTranslateNoDeadKeysMask, &dead, 4, &count, output) == noErr &&
                        count == 1 && output[0] == [character characterAtIndex:0]) { chord.key = key; found = true; }
                }
                if (!found) throw Error("invalid_key", "Key is unavailable in the current keyboard layout. Use type_text for text.");
            }
            has_key = true;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
        if (begin == value.size()) throw Error("invalid_key", "A chord cannot end with a separator.");
    }
    if (!has_key) throw Error("invalid_key", "A chord requires a non-modifier key.");
    return chord;
}

void revoke_input() { release_pressed(true); }

void input_action(const std::string& action, const json& request, Observation& observation) {
    require_accessibility();
    if ((action == "type_text" || action == "press_key") && IsSecureEventInputEnabled())
        throw Error("secure_input", "macOS Secure Input is active. Leave the protected input before using keyboard tools.");
    const auto held = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
    if (held & (kCGEventFlagMaskCommand | kCGEventFlagMaskControl | kCGEventFlagMaskAlternate | kCGEventFlagMaskShift))
        throw Error("input_busy", "Release held modifier keys before Computer Use input.");
    for (CGMouseButton button : {kCGMouseButtonLeft, kCGMouseButtonRight, kCGMouseButtonCenter})
        if (CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, button))
            throw Error("input_busy", "Release held mouse buttons before Computer Use input.");
    focus_check(observation, action == "type_text" || action == "press_key");
    struct Releases { ~Releases() { release_pressed(false); } } releases;
    if (action == "set_value" || action == "perform_secondary_action") {
        auto& element = selected_element(request, observation);
        if (element.secure) throw Error("protected_control", "Protected controls do not support accessibility value operations.");
        if (action == "set_value") {
            if (!request.contains("value") || !request["value"].is_string() || request["value"].get_ref<const std::string&>().size() > 16384)
                throw Error("invalid_argument", "value must be a string of at most 16 KiB.");
            if (!settable(element.ax.get(), kAXValueAttribute)) throw Error("unsupported_action", "This control has no writable value.");
            auto value = ns(request["value"].get<std::string>());
            if (!value) throw Error("invalid_text", "Value must contain valid UTF-8.");
            ax_check(AXUIElementSetAttributeValue(element.ax.get(), kAXValueAttribute, (__bridge CFStringRef)value), "Set control value");
        } else {
            const auto secondary = request.value("secondary_action", std::string{});
            if (std::find(element.actions.begin(), element.actions.end(), secondary) == element.actions.end())
                throw Error("unsupported_action", "Use only secondary actions advertised for this control.");
            if (secondary == "focus" || secondary == "select" || secondary == "expand" || secondary == "collapse") {
                const auto attribute = secondary == "focus" ? kAXFocusedAttribute : secondary == "select" ? kAXSelectedAttribute : kAXExpandedAttribute;
                ax_check(AXUIElementSetAttributeValue(element.ax.get(), attribute, secondary == "collapse" ? kCFBooleanFalse : kCFBooleanTrue), "Change control state");
            } else {
                CFStringRef native = kAXPressAction;
                if (secondary == "raise") native = kAXRaiseAction;
                else if (secondary == "scroll up") native = CFSTR("AXScrollUpByPage");
                else if (secondary == "scroll down") native = CFSTR("AXScrollDownByPage");
                else if (secondary == "scroll left") native = CFSTR("AXScrollLeftByPage");
                else if (secondary == "scroll right") native = CFSTR("AXScrollRightByPage");
                ax_check(AXUIElementPerformAction(element.ax.get(), native), "Perform control action");
            }
        }
        pointer_show(CGPointMake(CGRectGetMidX(element.rect), CGRectGetMidY(element.rect)));
    } else if (action == "press_key") {
        const auto chord = parse_key_chord(request.value("key", std::string{}));
        for (auto key : chord.modifiers) {
            Ref<CGEventRef> event(CGEventCreateKeyboardEvent(nullptr, key, true));
            if (event) CGEventSetFlags(event.get(), chord.flags);
            post(event.get(), key, -1, true);
        }
        for (bool down : {true, false}) {
            Ref<CGEventRef> event(CGEventCreateKeyboardEvent(nullptr, chord.key, down));
            if (event) CGEventSetFlags(event.get(), chord.flags);
            post(event.get(), chord.key, -1, down);
        }
    } else if (action == "type_text") {
        if (!request.contains("text") || !request["text"].is_string() || request["text"].get_ref<const std::string&>().size() > 16384)
            throw Error("invalid_argument", "text must be valid UTF-8 of at most 16 KiB.");
        NSString* value = ns(request["text"].get<std::string>());
        if (!value) throw Error("invalid_text", "Text must contain valid UTF-8.");
        for (NSUInteger offset = 0; offset < value.length;) {
            focus_check(observation, true);
            NSUInteger count = std::min<NSUInteger>(32, value.length - offset);
            if (offset + count < value.length && CFStringIsSurrogateHighCharacter([value characterAtIndex:offset + count - 1])) --count;
            UniChar chars[32]; [value getCharacters:chars range:NSMakeRange(offset, count)];
            for (bool down : {true, false}) {
                Ref<CGEventRef> event(CGEventCreateKeyboardEvent(nullptr, 0, down));
                if (event) CGEventKeyboardSetUnicodeString(event.get(), count, chars);
                post(event.get(), 0, -1, down);
            }
            offset += count;
        }
    } else {
        Element* element = nullptr;
        if (request.contains("element_index")) {
            if (action != "click" || request.contains("x") || request.contains("y")) throw Error("invalid_target", "Use an element OR screenshot coordinates for clicking.");
            element = &selected_element(request, observation);
        }
        const auto index = element ? element->surface : selected_surface(request, observation);
        const auto& surface = observation.surfaces[index];
        CGPoint point = element ? CGPointMake(CGRectGetMidX(element->rect), CGRectGetMidY(element->rect)) :
            coordinate(request, action == "drag" ? "from_x" : "x", action == "drag" ? "from_y" : "y", surface);
        validate_hit(point, surface, element ? element->ax.get() : nullptr);
        if (action == "click") {
            auto name = request.value("mouse_button", std::string("left"));
            if (name != "left" && name != "right" && name != "middle") throw Error("invalid_button", "Expected left, right or middle.");
            const auto button = name == "left" ? kCGMouseButtonLeft : name == "right" ? kCGMouseButtonRight : kCGMouseButtonCenter;
            if (request.contains("click_count") && !request["click_count"].is_number_integer()) throw Error("invalid_count", "click_count must be 1 or 2.");
            const int count = request.value("click_count", 1);
            if (count != 1 && count != 2) throw Error("invalid_count", "click_count must be 1 or 2.");
            for (int i = 1; i <= count; ++i) {
                pointer_show(point, true);
                mouse(button == 0 ? kCGEventLeftMouseDown : button == 1 ? kCGEventRightMouseDown : kCGEventOtherMouseDown, point, button, true, i);
                mouse(button == 0 ? kCGEventLeftMouseUp : button == 1 ? kCGEventRightMouseUp : kCGEventOtherMouseUp, point, button, false, i);
                if (i < count) std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            pointer_show(point);
        } else if (action == "scroll") {
            const double x = number(request, "scrollX"), y = number(request, "scrollY");
            if (std::abs(x) > 10000 || std::abs(y) > 10000) throw Error("invalid_scroll", "Each scroll delta must be within 10000 pixels.");
            Ref<CGEventRef> event(CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 2,
                static_cast<int>(-std::round(y)), static_cast<int>(-std::round(x))));
            if (event) CGEventSetLocation(event.get(), point);
            post(event.get()); pointer_show(point);
        } else if (action == "drag") {
            const auto target = coordinate(request, "to_x", "to_y", surface);
            validate_hit(target, surface);
            mouse(kCGEventLeftMouseDown, point, kCGMouseButtonLeft, true);
            for (int step = 1; step <= 24; ++step) {
                focus_check(observation, false);
                CGPoint intermediate{point.x + (target.x - point.x) * step / 24.0, point.y + (target.y - point.y) * step / 24.0};
                mouse(kCGEventLeftMouseDragged, intermediate, kCGMouseButtonLeft, true);
                pointer_show(intermediate, true);
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
            }
            mouse(kCGEventLeftMouseUp, target, kCGMouseButtonLeft, false); pointer_show(target);
        } else throw Error("unsupported_action", "Unknown input action.");
    }
}
} // namespace acecode::computer_use::macos
