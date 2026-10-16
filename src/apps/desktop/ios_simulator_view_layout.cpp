#include "ios_simulator_view_layout.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace acecode::desktop {
namespace {

bool read_int(const nlohmann::json& object, const char* key, int& out) {
    const auto found = object.find(key);
    if (found == object.end()) return true;
    if (!found->is_number()) return false;
    const double value = found->get<double>();
    if (value < -1e6 || value > 1e6) return false;
    out = static_cast<int>(value);
    return true;
}

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

} // namespace

bool is_simulator_udid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash ? value[index] != '-' : !std::isxdigit(static_cast<unsigned char>(value[index]))) return false;
    }
    return true;
}

std::optional<IosSimulatorViewLayout> parse_ios_simulator_view_layout(const nlohmann::json& value,
                                                                      std::string* error) {
    if (!value.is_object()) {
        fail(error, "layout must be an object");
        return std::nullopt;
    }
    IosSimulatorViewLayout layout;
    layout.udid = value.value("udid", std::string());
    layout.developer_dir = value.value("developer_dir", std::string());
    if (!is_simulator_udid(layout.udid)) {
        fail(error, "layout udid must be a simulator UDID");
        return std::nullopt;
    }
    if (layout.developer_dir.empty() || layout.developer_dir.front() != '/') {
        fail(error, "layout developer_dir must be an absolute path");
        return std::nullopt;
    }
    if (!read_int(value, "x", layout.frame.x) || !read_int(value, "y", layout.frame.y) ||
        !read_int(value, "width", layout.frame.width) || !read_int(value, "height", layout.frame.height)) {
        fail(error, "layout frame values must be numbers");
        return std::nullopt;
    }
    layout.frame.width = std::max(0, layout.frame.width);
    layout.frame.height = std::max(0, layout.frame.height);
    if (const auto visible = value.find("visible"); visible != value.end() && visible->is_boolean())
        layout.visible = visible->get<bool>();
    if (const auto radius = value.find("corner_radius"); radius != value.end()) {
        if (!radius->is_number()) {
            fail(error, "corner_radius must be a number");
            return std::nullopt;
        }
        layout.corner_radius = std::clamp(radius->get<double>(), 0.0, 400.0);
    }
    if (const auto revision = value.find("layout_revision"); revision != value.end()) {
        if (!revision->is_number_integer() || revision->get<std::int64_t>() < 0) {
            fail(error, "layout_revision must be a non-negative integer");
            return std::nullopt;
        }
        layout.layout_revision = revision->get<std::uint64_t>();
    }
    if (const auto rects = value.find("occlusion_rects"); rects != value.end()) {
        if (!rects->is_array() || rects->size() > kIosSimulatorViewMaxOcclusionRects) {
            fail(error, "occlusion_rects must be a bounded array");
            return std::nullopt;
        }
        for (const auto& item : *rects) {
            IosSimulatorViewRect rect;
            if (!item.is_object() || !read_int(item, "x", rect.x) || !read_int(item, "y", rect.y) ||
                !read_int(item, "width", rect.width) || !read_int(item, "height", rect.height)) {
                fail(error, "occlusion rectangle must be an object of numbers");
                return std::nullopt;
            }
            layout.occlusion_rects.push_back(rect);
        }
    }
    return layout;
}

std::vector<IosSimulatorViewRect> clipped_occlusion_rects(const IosSimulatorViewLayout& layout) {
    std::vector<IosSimulatorViewRect> out;
    const int width = layout.frame.width;
    const int height = layout.frame.height;
    for (const auto& rect : layout.occlusion_rects) {
        const int left = std::clamp(rect.x, 0, width);
        const int top = std::clamp(rect.y, 0, height);
        const int right = std::clamp(rect.x + std::max(0, rect.width), left, width);
        const int bottom = std::clamp(rect.y + std::max(0, rect.height), top, height);
        if (right > left && bottom > top) out.push_back({left, top, right - left, bottom - top});
    }
    return out;
}

IosSimulatorLayerGeometry fit_simulator_layer(double frame_width, double frame_height, double surface_width,
                                              double surface_height, int rotation) {
    IosSimulatorLayerGeometry geometry;
    rotation = ((rotation % 360) + 360) % 360;
    geometry.rotation = rotation == 90 || rotation == 180 || rotation == 270 ? rotation : 0;
    geometry.center_x = frame_width / 2;
    geometry.center_y = frame_height / 2;
    if (frame_width <= 0 || frame_height <= 0 || surface_width <= 0 || surface_height <= 0) return geometry;
    const bool quarter = geometry.rotation == 90 || geometry.rotation == 270;
    const double oriented_width = quarter ? surface_height : surface_width;
    const double oriented_height = quarter ? surface_width : surface_height;
    const double scale = std::min(frame_width / oriented_width, frame_height / oriented_height);
    geometry.width = surface_width * scale;
    geometry.height = surface_height * scale;
    return geometry;
}

double simulator_layer_angle_radians(int rotation, bool flipped_geometry) {
    const double radians = static_cast<double>(((rotation % 360) + 360) % 360) * std::acos(-1.0) / 180.0;
    return flipped_geometry ? radians : -radians;
}

} // namespace acecode::desktop
