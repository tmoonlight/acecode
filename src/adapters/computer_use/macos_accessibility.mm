#include "macos_native.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>

namespace acecode::computer_use::macos {
void ax_check(AXError error, const char* action) {
    if (error != kAXErrorSuccess)
        throw Error(error == kAXErrorAPIDisabled ? "accessibility_permission_required" : "accessibility_error",
            std::string(action) + " failed (AX error " + std::to_string(error) + "). Observe again.");
}
Ref<CFTypeRef> attribute(AXUIElementRef element, CFStringRef name) {
    CFTypeRef value = nullptr;
    if (element) AXUIElementCopyAttributeValue(element, name, &value);
    return Ref<CFTypeRef>(value);
}
AX ax_attribute(AXUIElementRef element, CFStringRef name) {
    auto value = attribute(element, name);
    if (!value || CFGetTypeID(value.get()) != AXUIElementGetTypeID()) return {};
    return AX::retained(static_cast<AXUIElementRef>(value.get()));
}
std::string string_attribute(AXUIElementRef element, CFStringRef name, std::size_t limit) {
    auto value = attribute(element, name);
    if (!value || CFGetTypeID(value.get()) != CFStringGetTypeID()) return {};
    NSString* string = (__bridge NSString*)value.get();
    if (string.length > limit) {
        NSUInteger count = limit;
        if (count && CFStringIsSurrogateHighCharacter([string characterAtIndex:count - 1])) --count;
        string = [string substringToIndex:count];
    }
    auto result = text(string);
    if (result.size() > limit) {
        std::size_t end = limit;
        while (end && (static_cast<unsigned char>(result[end]) & 0xc0) == 0x80) --end;
        result.resize(end);
    }
    return result;
}
bool bool_attribute(AXUIElementRef element, CFStringRef name, bool fallback) {
    auto value = attribute(element, name);
    return value && CFGetTypeID(value.get()) == CFBooleanGetTypeID()
        ? CFBooleanGetValue(static_cast<CFBooleanRef>(value.get())) : fallback;
}
bool settable(AXUIElementRef element, CFStringRef name) {
    Boolean result = false;
    return AXUIElementIsAttributeSettable(element, name, &result) == kAXErrorSuccess && result;
}
CGRect ax_bounds(AXUIElementRef element) {
    auto position = attribute(element, kAXPositionAttribute);
    auto size = attribute(element, kAXSizeAttribute);
    CGPoint point{}; CGSize extent{};
    if (!position || !size || CFGetTypeID(position.get()) != AXValueGetTypeID() ||
        CFGetTypeID(size.get()) != AXValueGetTypeID() ||
        !AXValueGetValue(static_cast<AXValueRef>(position.get()), kAXValueTypeCGPoint, &point) ||
        !AXValueGetValue(static_cast<AXValueRef>(size.get()), kAXValueTypeCGSize, &extent)) return CGRectNull;
    return CGRectMake(point.x, point.y, extent.width, extent.height);
}
std::vector<AX> ax_children(AXUIElementRef element, CFStringRef name) {
    std::vector<AX> result;
    // Read bounded slices; a pathological app must not allocate an unbounded tree.
    CFIndex count = 0;
    if (AXUIElementGetAttributeValueCount(element, name, &count) != kAXErrorSuccess || count <= 0) return result;
    CFArrayRef raw = nullptr;
    if (AXUIElementCopyAttributeValues(element, name, 0, std::min<CFIndex>(512, count), &raw) != kAXErrorSuccess || !raw) return result;
    Ref<CFArrayRef> values(raw);
    for (CFIndex i = 0; i < CFArrayGetCount(raw); ++i) {
        auto child = CFArrayGetValueAtIndex(raw, i);
        if (child && CFGetTypeID(child) == AXUIElementGetTypeID())
            result.push_back(AX::retained(static_cast<AXUIElementRef>(child)));
    }
    return result;
}
bool ax_descendant(AXUIElementRef element, AXUIElementRef ancestor) {
    AX current = AX::retained(element);
    for (int depth = 0; current && depth < 32; ++depth) {
        if (CFEqual(current.get(), ancestor)) return true;
        current = ax_attribute(current.get(), kAXParentAttribute);
    }
    return false;
}

json geometry(const Surface& surface) {
    const auto rect = surface.window.rect;
    return {{"width", surface.width}, {"height", surface.height}, {"native_width", rect.size.width},
        {"native_height", rect.size.height}, {"originX", rect.origin.x}, {"originY", rect.origin.y},
        {"desktop_coordinate_space", "screen_points"},
        {"scaleX", surface.width > 0 ? rect.size.width / surface.width : 0},
        {"scaleY", surface.height > 0 ? rect.size.height / surface.height : 0}};
}
json element_bounds(CGRect rect, const Surface& surface) {
    const auto bounds = surface.window.rect;
    return {{"x", (rect.origin.x - bounds.origin.x) * surface.width / bounds.size.width},
        {"y", (rect.origin.y - bounds.origin.y) * surface.height / bounds.size.height},
        {"width", rect.size.width * surface.width / bounds.size.width},
        {"height", rect.size.height * surface.height / bounds.size.height}};
}

json accessibility(Observation& observation) {
    json result{{"elements", json::array()}, {"selected_elements", json::array()}, {"truncated", false}};
    std::ostringstream tree;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    std::vector<AX> visited;
    int document_priority = 0;
    std::size_t text_budget = 24000;
    std::function<void(AX, std::size_t, int)> visit = [&](AX element, std::size_t surface_index, int depth) {
        if (!element) return;
        if (depth > 24 || visited.size() >= 400 || text_budget == 0 || Clock::now() >= deadline) {
            result["truncated"] = true; return;
        }
        if (std::any_of(visited.begin(), visited.end(), [&](const AX& value) { return CFEqual(value.get(), element.get()); })) return;
        visited.push_back(element);
        const auto role = string_attribute(element.get(), kAXRoleAttribute, 80);
        const auto subrole = string_attribute(element.get(), kAXSubroleAttribute, 80);
        const bool secure = subrole == "AXSecureTextField" || role == "AXSecureTextField";
        const auto rect = ax_bounds(element.get());
        const auto& surface = observation.surfaces.at(surface_index);
        if (!CGRectIsNull(rect) && CGRectIntersectsRect(rect, surface.window.rect)) {
            const bool enabled = bool_attribute(element.get(), kAXEnabledAttribute, true);
            const bool focused = observation.focus && CFEqual(observation.focus.get(), element.get());
            auto label = secure ? std::string("[protected]") : string_attribute(element.get(), kAXTitleAttribute, std::min<std::size_t>(1024, text_budget));
            if (label.empty() && !secure) label = string_attribute(element.get(), kAXDescriptionAttribute, std::min<std::size_t>(1024, text_budget));
            auto value = secure ? std::string() : string_attribute(element.get(), kAXValueAttribute, std::min<std::size_t>(4096, text_budget));
            const auto used = label.size() + value.size();
            text_budget -= std::min(text_budget, used);
            json actions = json::array();
            CFArrayRef raw_actions = nullptr;
            AXUIElementCopyActionNames(element.get(), &raw_actions);
            Ref<CFArrayRef> native_actions(raw_actions);
            const auto supports = [&](CFStringRef action) { return raw_actions && CFArrayContainsValue(raw_actions, CFRangeMake(0, CFArrayGetCount(raw_actions)), action); };
            if (enabled) {
                if (supports(kAXPressAction)) actions.push_back("invoke");
                if ((role == "AXCheckBox" || role == "AXSwitch") && supports(kAXPressAction)) actions.push_back("toggle");
                if (settable(element.get(), kAXSelectedAttribute)) actions.push_back("select");
                if (settable(element.get(), kAXExpandedAttribute)) { actions.push_back("expand"); actions.push_back("collapse"); }
                if (settable(element.get(), kAXFocusedAttribute)) actions.push_back("focus");
                if (supports(kAXRaiseAction)) actions.push_back("raise");
                for (const auto& pair : std::vector<std::pair<const char*, CFStringRef>>{
                    {"scroll up", CFSTR("AXScrollUpByPage")}, {"scroll down", CFSTR("AXScrollDownByPage")},
                    {"scroll left", CFSTR("AXScrollLeftByPage")}, {"scroll right", CFSTR("AXScrollRightByPage")}})
                    if (supports(pair.second)) actions.push_back(pair.first);
            }
            const auto index = observation.elements.size();
            observation.elements.push_back({element, rect, surface_index, secure, actions});
            json item{{"index", index}, {"role", role}, {"name", label}, {"value", value},
                {"bounds", element_bounds(rect, surface)}, {"enabled", enabled}, {"focused", focused},
                {"password", secure}, {"screenshot_id", surface.screenshot_id}, {"actions", actions},
                {"settable", enabled && !secure && settable(element.get(), kAXValueAttribute)}};
            if (!secure && value.empty()) {
                auto native_value = attribute(element.get(), kAXValueAttribute);
                if (native_value && CFGetTypeID(native_value.get()) == CFBooleanGetTypeID())
                    item["value"] = CFBooleanGetValue(static_cast<CFBooleanRef>(native_value.get()));
                else if (native_value && CFGetTypeID(native_value.get()) == CFNumberGetTypeID()) {
                    double number = 0;
                    if (CFNumberGetValue(static_cast<CFNumberRef>(native_value.get()), kCFNumberDoubleType, &number) && std::isfinite(number))
                        item["value"] = number;
                }
            }
            result["elements"].push_back(item);
            tree << std::string(std::min(depth, 12) * 2, ' ') << index << " " << role << " " << label;
            if (!value.empty()) tree << " value=" << value;
            tree << '\n';
            if (focused) {
                result["focused_element"] = item;
                if (!secure) result["selected_text"] = string_attribute(element.get(), kAXSelectedTextAttribute, 4096);
            }
            if (bool_attribute(element.get(), kAXSelectedAttribute)) result["selected_elements"].push_back(item);
            const int priority = secure ? 0 : focused ? 2 : (role == "AXTextArea" || role == "AXWebArea") ? 1 : 0;
            if (priority > document_priority && !value.empty()) { result["document_text"] = value; document_priority = priority; }
        }
        if (!secure) for (auto child : ax_children(element.get())) visit(child, surface_index, depth + 1);
    };
    for (std::size_t i = observation.surfaces.size(); i > 0; --i) visit(observation.surfaces[i - 1].window.root, i - 1, 0);
    result["tree"] = tree.str();
    return result;
}
} // namespace acecode::computer_use::macos
