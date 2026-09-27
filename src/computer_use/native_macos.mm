#include "native_backend.hpp"
#include "macos_native.hpp"
#include "config/vocab/pointer_appearance.hpp"

#include <set>

namespace acecode::computer_use {
using namespace macos;
namespace {
std::string required_text(const json& request, const char* key, std::size_t maximum = 16384) {
    if (!request.contains(key) || !request[key].is_string()) throw Error("invalid_argument", std::string(key) + " must be a string.");
    auto value = request[key].get<std::string>();
    if (value.empty() || value.size() > maximum || value.find('\0') != std::string::npos)
        throw Error("invalid_argument", std::string(key) + " is empty or exceeds its allowed size.");
    return value;
}
CGWindowID window_id(const json& request) {
    if (!request.contains("window") || !request["window"].is_number_integer()) throw Error("invalid_window", "window must be an observed integer id.");
    const auto value = request["window"].get<std::int64_t>();
    if (value <= 0 || value > UINT32_MAX) throw Error("invalid_window", "Invalid window id.");
    return static_cast<CGWindowID>(value);
}
}

struct NativeBackend::Impl {
    Observation observed;
    std::string nonce = text(NSUUID.UUID.UUIDString);
    std::uint64_t generation = 0;

    Impl() {
        AX system(AXUIElementCreateSystemWide());
        AXUIElementSetMessagingTimeout(system.get(), 0.25f);
    }
    ~Impl() { pointer_hide(); }

    json observe(const json& request) {
        observed = {};
        require_accessibility(); require_interactive_session();
        for (const char* key : {"include_screenshot", "include_text"})
            if (request.contains(key) && !request[key].is_boolean()) throw Error("invalid_argument", std::string(key) + " must be a boolean.");
        const auto root = window(window_id(request));
        Observation next;
        next.surfaces = related_surfaces(root);
        next.id = nonce + "-" + std::to_string(++generation);
        next.foreground = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
        AX app(AXUIElementCreateApplication(root.pid));
        next.focus = ax_attribute(app.get(), kAXFocusedUIElementAttribute);
        json output{{"window", describe(root)}, {"observation_id", next.id}, {"coordinate_space", "screenshot_pixels"},
            {"screenshots", json::array()}, {"surfaces", json::array()}, {"accessibility", nullptr},
            {"instruction", "Use this observation_id with the original window id for one action, then observe again. For multiple images, select screenshot_id explicitly. Each image and accessibility element has its own surface coordinates."}};
        json attachments = json::array(), warnings = json::array();
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < next.surfaces.size(); ++i) {
            auto& surface = next.surfaces[i];
            auto size = capture_size(screen_rect(surface.window.rect), 1);
            surface.width = size.first; surface.height = size.second;
            json descriptor = geometry(surface);
            descriptor.update({{"window", surface.window.id}, {"relation", surface.relation}});
            if (request.value("include_screenshot", true)) {
                try {
                    auto image = capture(surface, next.id, i, descriptor);
                    bytes += image["data_url"].get_ref<const std::string&>().size();
                    if (bytes > 24 * 1024 * 1024) throw Error("capture_budget", "Combined screenshots exceed the 24 MiB budget.");
                    attachments.push_back(std::move(image));
                    output["screenshots"].push_back(descriptor);
                } catch (const Error& error) {
                    surface.screenshot_id.clear();
                    descriptor.erase("id");
                    descriptor["capture_error"] = error.code;
                    warnings.push_back({{"window", surface.window.id}, {"error", error.code}, {"message", error.what()}});
                }
            }
            output["surfaces"].push_back(std::move(descriptor));
        }
        output["geometry"] = geometry(next.surfaces.front());
        if (request.value("include_text", true)) output["accessibility"] = accessibility(next);
        if (request.value("include_screenshot", true) && attachments.empty() && next.elements.empty())
            throw Error("observation_unavailable", warnings.empty() ? "No requested window content is available." : warnings[0]["message"].get<std::string>());
        validate_surfaces(next);
        auto current_focus = ax_attribute(app.get(), kAXFocusedUIElementAttribute);
        if (NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier != next.foreground ||
            static_cast<bool>(current_focus) != static_cast<bool>(next.focus) ||
            (next.focus && !CFEqual(next.focus.get(), current_focus.get())))
            throw Error("focus_changed", "Focus changed during observation. Observe again.");
        if (!warnings.empty()) output["warnings"] = warnings;
        next.time = Clock::now();
        observed = std::move(next);
        return {{"success", true}, {"output", output}, {"attachments", attachments}};
    }

    json dispatch(const json& request) {
        const auto action = required_text(request, "action", 80);
        auto appearance = request.value("pointer_appearance", json::object());
        if (appearance.is_object()) pointer_configure(appearance.value("style", std::string("ace")), appearance.value("color", std::string("#2563eb")));
        if (action == "release") { observed = {}; pointer_hide(); return {{"success", true}, {"output", "Desktop observation released."}}; }
        if (action == "list_apps") return {{"success", true}, {"output", {{"apps", list_apps()}}}};
        if (action == "list_windows") {
            json output = json::array();
            for (const auto& value : windows()) output.push_back(describe(value));
            return {{"success", true}, {"output", {{"windows", output}}}};
        }
        if (action == "get_window") {
            const auto value = window(window_id(request), false);
            if (request.contains("app")) {
                const auto expected = required_text(request, "app");
                const auto application = [NSRunningApplication runningApplicationWithProcessIdentifier:value.pid];
                if (value.app != expected && text(application.bundleURL.path) != expected)
                    throw Error("window_app_mismatch", "Window no longer belongs to the requested application.");
            }
            return {{"success", true}, {"output", {{"window", describe(value)}, {"requires_observation", true}}}};
        }
        if (action == "launch_app") { observed = {}; require_interactive_session(); return {{"success", true}, {"output", launch_app(required_text(request, "app"))}}; }
        if (action == "activate_window") {
            observed = {};
            const auto value = window(window_id(request));
            activate(value);
            return {{"success", true}, {"output", {{"window", describe(value)}, {"requires_observation", true}}}};
        }
        if (action == "get_window_state") return observe(request);
        static const std::set<std::string> actions{"click", "type_text", "press_key", "scroll", "drag", "set_value", "perform_secondary_action"};
        if (!actions.count(action)) throw Error("unsupported_action", "Unknown Computer Use action.");
        const auto id = required_text(request, "observation_id", 256);
        if (id != observed.id || observed.id.empty()) throw Error("stale_observation", "Observation is missing or already used. Observe again.");
        observed.id.clear();
        if (observed.surfaces.empty() || window_id(request) != observed.surfaces.front().window.id ||
            Clock::now() - observed.time > std::chrono::seconds(90))
            throw Error("stale_observation", "Window or observation age changed. Observe again.");
        require_accessibility(); require_interactive_session();
        validate_surfaces(observed);
        input_action(action, request, observed);
        return {{"success", true}, {"output", {{"action", action}, {"window", describe(observed.surfaces.front().window)}, {"requires_observation", true}}}};
    }
};

NativeBackend::NativeBackend() : impl_(std::make_unique<Impl>()) {}
NativeBackend::~NativeBackend() = default;
json NativeBackend::dispatch(const json& request) {
    @autoreleasepool {
        try {
            if (!request.is_object()) throw Error("invalid_argument", "Computer Use request must be an object.");
            return impl_->dispatch(request);
        } catch (const Error& error) {
            return {{"success", false}, {"error", error.code}, {"output", {{"error", error.code}, {"message", error.what()}}}};
        } catch (const std::exception& error) {
            return {{"success", false}, {"error", "invalid_argument"}, {"output", {{"error", "invalid_argument"}, {"message", error.what()}}}};
        }
    }
}
} // namespace acecode::computer_use
