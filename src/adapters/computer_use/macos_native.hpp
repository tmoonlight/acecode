#pragma once

#if defined(__APPLE__) && defined(__OBJC__)
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "macos_geometry.hpp"
#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace acecode::computer_use::macos {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Error : std::runtime_error {
    std::string code;
    Error(std::string code, const std::string& message) : std::runtime_error(message), code(std::move(code)) {}
};

template<class T> class Ref {
public:
    Ref(T value = nullptr) : value_(value) {}
    Ref(const Ref& other) : value_(other.value_) { if (value_) CFRetain(value_); }
    Ref(Ref&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    Ref& operator=(Ref other) { std::swap(value_, other.value_); return *this; }
    ~Ref() { if (value_) CFRelease(value_); }
    T get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    static Ref retained(T value) { if (value) CFRetain(value); return Ref(value); }
private:
    T value_;
};
using AX = Ref<AXUIElementRef>;

inline std::string text(NSString* value) { return value.UTF8String ?: ""; }
inline NSString* ns(const std::string& value) { return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding]; }
inline ScreenRect screen_rect(CGRect rect) { return {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height}; }
inline CGRect cg_rect(ScreenRect rect) { return CGRectMake(rect.x, rect.y, rect.width, rect.height); }
inline bool same_rect(CGRect a, CGRect b) {
    return std::abs(a.origin.x - b.origin.x) < 0.5 && std::abs(a.origin.y - b.origin.y) < 0.5 &&
        std::abs(a.size.width - b.size.width) < 0.5 && std::abs(a.size.height - b.size.height) < 0.5;
}

Ref<CFTypeRef> attribute(AXUIElementRef element, CFStringRef name);
AX ax_attribute(AXUIElementRef element, CFStringRef name);
std::string string_attribute(AXUIElementRef element, CFStringRef name, std::size_t limit = 4096);
bool bool_attribute(AXUIElementRef element, CFStringRef name, bool fallback = false);
bool settable(AXUIElementRef element, CFStringRef name);
CGRect ax_bounds(AXUIElementRef element);
std::vector<AX> ax_children(AXUIElementRef element, CFStringRef name = kAXChildrenAttribute);
bool ax_descendant(AXUIElementRef element, AXUIElementRef ancestor);
void ax_check(AXError error, const char* action);

struct Window {
    CGWindowID id = 0;
    pid_t pid = 0;
    std::uint64_t process_start = 0;
    std::string app, title;
    CGRect rect{};
    AX root;
};
struct Surface {
    Window window;
    std::string relation = "main", screenshot_id;
    int width = 0, height = 0;
};
struct Element {
    AX ax;
    CGRect rect{};
    std::size_t surface = 0;
    bool secure = false;
    json actions = json::array();
};
struct Observation {
    std::string id;
    Clock::time_point time;
    std::vector<Surface> surfaces;
    std::vector<Element> elements;
    AX focus;
    pid_t foreground = 0;
};

json permissions(const std::string& request = {});
void require_accessibility();
void require_interactive_session();
std::vector<Window> windows(bool bind_ax = false);
Window window(CGWindowID id, bool bind_ax = true);
json describe(const Window& value);
json list_apps();
json launch_app(const std::string& application);
void activate(const Window& value);
std::vector<Surface> related_surfaces(const Window& root);
json geometry(const Surface& surface);
json element_bounds(CGRect rect, const Surface& surface);
json accessibility(Observation& observation);
json capture(Surface& surface, const std::string& observation_id, std::size_t index, json& descriptor);
void validate_surfaces(const Observation& observation);
void validate_hit(CGPoint point, const Surface& surface, AXUIElementRef element = nullptr);
void input_action(const std::string& action, const json& request, Observation& observation);
void revoke_input();

void pointer_configure(const std::string& style, const std::string& color);
void pointer_show(CGPoint point, bool pressed = false);
void pointer_hide();
bool pointer_owns(CGWindowID id);
json pointer_composite(CGContextRef context, const Surface& surface);
} // namespace acecode::computer_use::macos
#endif
