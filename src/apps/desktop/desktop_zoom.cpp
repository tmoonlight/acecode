#include "desktop_zoom.hpp"

#include "utils/logger.hpp"
#include "utils/state_file.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace acecode::desktop {

namespace {

constexpr const char* kStateKey = "desktop_zoom_factor";

std::string bridge_result(bool ok, double factor, const char* error = nullptr) {
    nlohmann::json result{{"ok", ok}, {"factor", factor}};
    if (error) result["error"] = error;
    return result.dump();
}

} // namespace

double clamp_desktop_zoom_factor(double factor) {
    if (!std::isfinite(factor)) return 1.0;
    return std::clamp(factor, kMinDesktopZoomFactor, kMaxDesktopZoomFactor);
}

DesktopZoom::DesktopZoom(Apply apply) : apply_(std::move(apply)) {}

void DesktopZoom::restore(std::optional<double> saved) {
    const double factor = clamp_desktop_zoom_factor(saved.value_or(1.0));
    supported_ = apply_ && apply_(factor);
    factor_ = supported_ ? factor : 1.0;
    restored_ = factor_;
}

std::string DesktopZoom::handle_bridge(const std::string& request) {
    if (!supported_) return bridge_result(false, factor_, "page zoom unsupported");
    nlohmann::json args;
    try {
        args = nlohmann::json::parse(request);
    } catch (...) {
        return bridge_result(false, factor_, "expect [] or [factor]");
    }
    if (!args.is_array()) return bridge_result(false, factor_, "expect [] or [factor]");
    if (args.empty()) return bridge_result(true, factor_);
    if (!args[0].is_number()) return bridge_result(false, factor_, "expect [factor]");

    const double next = clamp_desktop_zoom_factor(args[0].get<double>());
    if (!apply_(next)) return bridge_result(false, factor_, "apply page zoom failed");
    factor_ = next;
    return bridge_result(true, factor_);
}

std::optional<double> read_desktop_zoom_factor() {
    const auto state = read_state_json();
    const auto it = state.find(kStateKey);
    if (it == state.end() || !it->is_number()) return std::nullopt;
    const double factor = it->get<double>();
    if (!std::isfinite(factor)) return std::nullopt;
    return clamp_desktop_zoom_factor(factor);
}

void write_desktop_zoom_factor(double factor) {
    const double value = clamp_desktop_zoom_factor(factor);
    if (!update_state_json([value](nlohmann::json& state) {
            state[kStateKey] = value;
            return true;
        })) {
        LOG_WARN("[desktop] failed to save page zoom to state.json");
    }
}

} // namespace acecode::desktop
