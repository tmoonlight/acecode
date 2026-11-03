#include "window_bounds_state.hpp"

#include "utils/logger.hpp"
#include "utils/state_file.hpp"

namespace acecode::desktop {

namespace {

constexpr const char* kStateKey = "desktop_window";
// 系统允许的缩放是 100%–500%,再留些余量;超出范围的值只可能是坏数据。
constexpr int kMinSavedDpi = 48;
constexpr int kMaxSavedDpi = 960;
// 坐标远超任何真实显示器布局时视为坏数据,避免换算时溢出。
constexpr long long kMaxSavedCoordinate = 1'000'000;

std::optional<int> coordinate(const nlohmann::json& value, const char* key) {
    const auto it = value.find(key);
    if (it == value.end() || !it->is_number_integer()) return std::nullopt;
    const long long number = it->get<long long>();
    if (number < -kMaxSavedCoordinate || number > kMaxSavedCoordinate) return std::nullopt;
    return static_cast<int>(number);
}

} // namespace

std::optional<SavedDesktopWindowBounds> parse_desktop_window_bounds(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto left = coordinate(value, "left");
    const auto top = coordinate(value, "top");
    const auto right = coordinate(value, "right");
    const auto bottom = coordinate(value, "bottom");
    const auto dpi = coordinate(value, "dpi");
    if (!left || !top || !right || !bottom || !dpi) return std::nullopt;
    if (*right <= *left || *bottom <= *top) return std::nullopt;
    if (*dpi < kMinSavedDpi || *dpi > kMaxSavedDpi) return std::nullopt;

    SavedDesktopWindowBounds bounds;
    bounds.rect = {*left, *top, *right, *bottom};
    bounds.dpi = *dpi;
    const auto maximized = value.find("maximized");
    bounds.maximized = maximized != value.end() && maximized->is_boolean() &&
                       maximized->get<bool>();
    return bounds;
}

nlohmann::json desktop_window_bounds_json(const SavedDesktopWindowBounds& bounds) {
    return {
        {"left", bounds.rect.left},
        {"top", bounds.rect.top},
        {"right", bounds.rect.right},
        {"bottom", bounds.rect.bottom},
        {"dpi", bounds.dpi},
        {"maximized", bounds.maximized},
    };
}

std::optional<SavedDesktopWindowBounds> read_desktop_window_bounds() {
    const auto state = read_state_json();
    const auto it = state.find(kStateKey);
    if (it == state.end()) return std::nullopt;
    return parse_desktop_window_bounds(*it);
}

void write_desktop_window_bounds(const SavedDesktopWindowBounds& bounds) {
    const auto value = desktop_window_bounds_json(bounds);
    if (!update_state_json([&value](nlohmann::json& state) {
            state[kStateKey] = value;
            return true;
        })) {
        LOG_WARN("[desktop] failed to save window bounds to state.json");
    }
}

} // namespace acecode::desktop
