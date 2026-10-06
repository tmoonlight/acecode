#include "desktop_pet_layout.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace acecode::desktop {
namespace {

constexpr int kBaseDpi = 96;
constexpr int kEdgeMarginDip = 8;
constexpr double kMinLogicalScale = 0.5;
constexpr double kMaxWorkAreaFraction = 0.9;
// 命中区域比房间轮廓外扩的场景像素数:盖住描边和轮廓线的取整误差。
constexpr int kHitMargin = 3;

// 与 agent_office_pet.html 里的场景常量一致:VIEW 原点 (156, 54),
// 房间世界坐标 x∈[-3, 88]、y∈[-3, 72](含墙厚),地台底 z=-4,墙顶 z=40。
constexpr int kViewOriginX = 156;
constexpr int kViewOriginY = 54 + kDesktopPetToolbarHeight;
constexpr int kRoomMinX = -3;
constexpr int kRoomMaxX = 88;
constexpr int kRoomMinY = -3;
constexpr int kRoomMaxY = 72;
constexpr int kRoomBottomZ = -4;
constexpr int kWallTopZ = 40;

int effective_dpi(int dpi) {
    return dpi > 0 ? dpi : kBaseDpi;
}

// 等距投影,同页面里的 P():屏幕 x = ox + 2(x − y),屏幕 y = oy + x + y − z。
DesktopPetPoint project(int x, int y, int z) {
    return {kViewOriginX + 2 * (x - y), kViewOriginY + x + y - z};
}

DesktopPetRect sized_at(int x, int y, double scale) {
    return {x, y,
            static_cast<int>(std::lround(kDesktopPetSceneWidth * scale)),
            static_cast<int>(std::lround(kDesktopPetSceneHeight * scale))};
}

int clamp_origin(int value, int size, int area_start, int area_size) {
    const int max_value = area_start + area_size - size;
    return std::max(area_start, std::min(value, max_value));
}

double scale_for(int dpi, const DesktopPetRect& work_area, double preferred_logical_scale) {
    if (preferred_logical_scale > 0.0) {
        return clamp_desktop_pet_scale(
            preferred_logical_scale * effective_dpi(dpi) / kBaseDpi, dpi, work_area);
    }
    return desktop_pet_scale(dpi, work_area.width, work_area.height);
}

} // namespace

int desktop_pet_scale(int dpi, int work_width, int work_height) {
    // round(dpi / 96 × 1.25),整数运算避免浮点边界抖动。
    int scale = std::max(1, (effective_dpi(dpi) * 125 + 4800) / 9600);
    while (scale > 1 &&
           (kDesktopPetSceneWidth * scale > work_width / 2 ||
            kDesktopPetSceneHeight * scale > work_height / 2)) {
        --scale;
    }
    return scale;
}

double clamp_desktop_pet_scale(double scale, int dpi, const DesktopPetRect& work_area) {
    const double min_scale = kMinLogicalScale * effective_dpi(dpi) / kBaseDpi;
    double max_scale = std::min(
        work_area.width * kMaxWorkAreaFraction / kDesktopPetSceneWidth,
        work_area.height * kMaxWorkAreaFraction / kDesktopPetSceneHeight);
    if (!(max_scale > min_scale)) max_scale = min_scale;   // 工作区异常小时只保证下限
    if (!std::isfinite(scale)) return min_scale;
    return std::clamp(scale, min_scale, max_scale);
}

DesktopPetPlacement place_desktop_pet(const DesktopPetRect& work_area, int dpi,
                                      double preferred_logical_scale) {
    DesktopPetPlacement placement;
    placement.scale = scale_for(dpi, work_area, preferred_logical_scale);
    const int margin = (kEdgeMarginDip * effective_dpi(dpi) + kBaseDpi / 2) / kBaseDpi;
    placement.window = sized_at(0, 0, placement.scale);
    placement.window.x = clamp_origin(
        work_area.x + work_area.width - placement.window.width - margin,
        placement.window.width, work_area.x, work_area.width);
    placement.window.y = clamp_origin(
        work_area.y + work_area.height - placement.window.height - margin,
        placement.window.height, work_area.y, work_area.height);
    return placement;
}

DesktopPetPlacement resize_desktop_pet(const DesktopPetRect& current, int dpi,
                                       const DesktopPetRect& work_area,
                                       double preferred_logical_scale) {
    DesktopPetPlacement placement;
    placement.scale = scale_for(dpi, work_area, preferred_logical_scale);
    placement.window = sized_at(current.x, current.y, placement.scale);
    placement.window.x = clamp_origin(placement.window.x, placement.window.width,
                                      work_area.x, work_area.width);
    placement.window.y = clamp_origin(placement.window.y, placement.window.height,
                                      work_area.y, work_area.height);
    return placement;
}

DesktopPetPlacement scale_desktop_pet(const DesktopPetRect& current, double new_scale,
                                      double anchor_fx, double anchor_fy,
                                      const DesktopPetRect& work_area) {
    anchor_fx = std::isfinite(anchor_fx) ? std::clamp(anchor_fx, 0.0, 1.0) : 1.0;
    anchor_fy = std::isfinite(anchor_fy) ? std::clamp(anchor_fy, 0.0, 1.0) : 1.0;
    const double anchor_x = current.x + anchor_fx * current.width;
    const double anchor_y = current.y + anchor_fy * current.height;
    DesktopPetPlacement placement;
    placement.scale = new_scale;
    placement.window = sized_at(0, 0, new_scale);
    placement.window.x = clamp_origin(
        static_cast<int>(std::lround(anchor_x - anchor_fx * placement.window.width)),
        placement.window.width, work_area.x, work_area.width);
    placement.window.y = clamp_origin(
        static_cast<int>(std::lround(anchor_y - anchor_fy * placement.window.height)),
        placement.window.height, work_area.y, work_area.height);
    return placement;
}

std::vector<DesktopPetPoint> desktop_pet_hit_polygon(double scale) {
    if (!(scale > 0.0)) scale = 1.0;
    // 房间外包六边形,从左墙顶端起顺时针:左墙顶 → 墙角顶 → 后墙右顶 →
    // 地台右角 → 地台前角 → 地台左角。每个点向外推 kHitMargin。
    const DesktopPetPoint wall_left = project(kRoomMinX, kRoomMaxY, kWallTopZ);
    const DesktopPetPoint wall_corner = project(kRoomMinX, kRoomMinY, kWallTopZ);
    const DesktopPetPoint wall_right = project(kRoomMaxX, kRoomMinY, kWallTopZ);
    const DesktopPetPoint floor_right = project(kRoomMaxX, kRoomMinY, kRoomBottomZ);
    const DesktopPetPoint floor_front = project(kRoomMaxX, kRoomMaxY, kRoomBottomZ);
    const DesktopPetPoint floor_left = project(kRoomMinX, kRoomMaxY, kRoomBottomZ);
    const std::vector<DesktopPetPoint> outline = {
        {wall_left.x - kHitMargin, wall_left.y - kHitMargin},
        {wall_corner.x, wall_corner.y - kHitMargin},
        {wall_right.x + kHitMargin, wall_right.y - kHitMargin},
        {floor_right.x + kHitMargin, floor_right.y + kHitMargin},
        {floor_front.x, floor_front.y + kHitMargin},
        {floor_left.x - kHitMargin, floor_left.y + kHitMargin},
    };
    std::vector<DesktopPetPoint> polygon;
    polygon.reserve(outline.size());
    for (const auto& point : outline) {
        polygon.push_back({
            static_cast<int>(std::lround(std::clamp(point.x, 0, kDesktopPetSceneWidth) * scale)),
            static_cast<int>(std::lround(std::clamp(point.y, 0, kDesktopPetSceneHeight) * scale)),
        });
    }
    return polygon;
}

namespace {
bool overlay_from_json(const nlohmann::json& value, DesktopPetOverlay& out) {
    if (!value.is_array() || value.size() != 4) return false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!value[i].is_number()) return false;
        const double coordinate = value[i].get<double>();
        if (!std::isfinite(coordinate)) return false;
        out[i] = std::clamp(coordinate, 0.0, 1.0);
    }
    return out[2] > out[0] && out[3] > out[1];
}
} // namespace

std::vector<DesktopPetOverlay> desktop_pet_overlays_from_message(const nlohmann::json& message) {
    std::vector<DesktopPetOverlay> overlays;
    if (!message.is_object()) return overlays;
    DesktopPetOverlay rect{};
    if (const auto it = message.find("rect"); it != message.end() && overlay_from_json(*it, rect)) {
        overlays.push_back(rect);
    }
    if (const auto it = message.find("bubbles"); it != message.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (overlays.size() >= kDesktopPetMaxOverlays) break;
            if (overlay_from_json(item, rect)) overlays.push_back(rect);
        }
    }
    return overlays;
}

bool desktop_pet_hit_test(double scale, int width, int height,
                          const std::vector<DesktopPetOverlay>& overlays, double x, double y) {
    if (width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height) return false;
    if (!(scale > 0.0)) scale = 1.0;
    if (y < kDesktopPetToolbarHeight * scale) return true;
    for (const auto& overlay : overlays) {
        if (x >= overlay[0] * width && x < overlay[2] * width &&
            y >= overlay[1] * height && y < overlay[3] * height) {
            return true;
        }
    }
    // 偶奇规则判断点是否在房间多边形里。
    const auto polygon = desktop_pet_hit_polygon(scale);
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const double xi = polygon[i].x, yi = polygon[i].y, xj = polygon[j].x, yj = polygon[j].y;
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside;
    }
    return inside;
}

} // namespace acecode::desktop
