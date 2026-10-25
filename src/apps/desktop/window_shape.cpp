#include "window_shape.hpp"

#include "window_background.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <nlohmann/json.hpp>

namespace acecode::desktop {

namespace {

struct Point {
    double x = 0;
    double y = 0;
};

// 八边形顶点(连续坐标),与 chamfer_row_inset 的像素判定同一条斜边:
// 左上斜边 x + y = cut,右上 x - y = width - cut,左下 y - x = height - cut,
// 右下 x + y = width + height - cut。
std::array<Point, 8> chamfer_vertices(int width, int height, int cut) {
    const double w = width;
    const double h = height;
    const double c = cut;
    return {{{c, 0}, {w - c, 0}, {w, c}, {w, h - c}, {w - c, h}, {c, h}, {0, h - c}, {0, c}}};
}

double segment_distance(Point p, Point a, Point b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length_sq = dx * dx + dy * dy;
    double t = 0;
    if (length_sq > 0) t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length_sq, 0.0, 1.0);
    const double ex = p.x - (a.x + t * dx);
    const double ey = p.y - (a.y + t * dy);
    return std::sqrt(ex * ex + ey * ey);
}

bool inside_octagon(double x, double y, int width, int height, int cut) {
    return x >= 0 && y >= 0 && x <= width && y <= height &&
           x + y >= cut && x - y <= width - cut && y - x <= height - cut &&
           x + y <= width + height - cut;
}

std::uint8_t to_byte(double value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

double falloff(double distance, double extent) {
    if (extent <= 0) return 0;
    const double t = std::clamp(distance / extent, 0.0, 1.0);
    return (1.0 - t) * (1.0 - t);
}

} // namespace

int chamfer_cut_px(int cut_dip, int dpi) {
    if (cut_dip <= 0) return 0;
    const int safe_dpi = dpi > 0 ? dpi : 96;
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(cut_dip) * safe_dpi / 96.0)));
}

int clamp_chamfer_cut(int width, int height, int cut) {
    if (width <= 0 || height <= 0 || cut <= 0) return 0;
    return std::min(cut, std::min(width, height) / 2);
}

int chamfer_row_inset(int y, int height, int cut) {
    if (cut <= 0 || y < 0 || y >= height) return 0;
    if (y < cut) return cut - 1 - y;
    if (y >= height - cut) return y - (height - cut);
    return 0;
}

std::vector<ShapeRect> chamfer_region_rects(int width, int height, int cut) {
    std::vector<ShapeRect> rects;
    if (width <= 0 || height <= 0) return rects;
    cut = clamp_chamfer_cut(width, height, cut);
    for (int y = 0; y < cut; ++y) {
        const int inset = chamfer_row_inset(y, height, cut);
        rects.push_back({inset, y, width - inset, y + 1});
    }
    if (height - 2 * cut > 0) rects.push_back({0, cut, width, height - cut});
    for (int y = std::max(cut, height - cut); y < height; ++y) {
        const int inset = chamfer_row_inset(y, height, cut);
        rects.push_back({inset, y, width - inset, y + 1});
    }
    return rects;
}

bool chamfer_contains_pixel(int x, int y, int width, int height, int cut) {
    if (x < 0 || y < 0 || x >= width || y >= height) return false;
    const int inset = chamfer_row_inset(y, height, clamp_chamfer_cut(width, height, cut));
    return x >= inset && x < width - inset;
}

double chamfer_outside_distance(double x, double y, int width, int height, int cut) {
    cut = clamp_chamfer_cut(width, height, cut);
    if (width <= 0 || height <= 0) return 0;
    if (inside_octagon(x, y, width, height, cut)) return 0;
    const auto vertices = chamfer_vertices(width, height, cut);
    double best = segment_distance({x, y}, vertices[7], vertices[0]);
    for (std::size_t i = 0; i + 1 < vertices.size(); ++i) {
        best = std::min(best, segment_distance({x, y}, vertices[i], vertices[i + 1]));
    }
    return best;
}

std::vector<ShadowStrip> chamfer_shadow_strips(int width, int height, int cut, int margin) {
    std::vector<ShadowStrip> strips;
    if (width <= 0 || height <= 0 || margin <= 0) return strips;
    cut = clamp_chamfer_cut(width, height, cut);
    strips.push_back({-margin, -margin, width + 2 * margin, margin + cut});
    strips.push_back({-margin, height - cut, width + 2 * margin, cut + margin});
    if (height - 2 * cut > 0) {
        strips.push_back({-margin, cut, margin, height - 2 * cut});
        strips.push_back({width, cut, margin, height - 2 * cut});
    }
    return strips;
}

void render_chamfer_shadow(std::uint32_t* pixels, const ShadowStrip& strip,
                           int width, int height, int cut, const ShadowStyle& style) {
    if (!pixels || strip.width <= 0 || strip.height <= 0) return;
    cut = clamp_chamfer_cut(width, height, cut);
    // 失焦窗口的阴影与辉光都变淡,和系统窗口的激活态差别一致。
    const double shadow_peak = style.active ? 0.30 : 0.16;
    const double glow_peak = style.active ? 0.55 : 0.22;
    for (int row = 0; row < strip.height; ++row) {
        const int y = strip.y + row;
        for (int column = 0; column < strip.width; ++column) {
            const int x = strip.x + column;
            std::uint32_t& out = pixels[static_cast<std::size_t>(row) * strip.width + column];
            if (chamfer_contains_pixel(x, y, width, height, cut)) {
                out = 0;
                continue;
            }
            const double cx = x + 0.5;
            const double cy = y + 0.5;
            const double shadow = shadow_peak *
                falloff(chamfer_outside_distance(cx, cy - style.offset_y, width, height, cut), style.margin);
            const double glow = style.glow_width > 0
                ? glow_peak * falloff(chamfer_outside_distance(cx, cy, width, height, cut), style.glow_width)
                : 0.0;
            // 辉光盖在黑色阴影上,结果按预乘 alpha 输出。
            const double alpha = glow + shadow * (1.0 - glow);
            out = (static_cast<std::uint32_t>(to_byte(alpha)) << 24) |
                  (static_cast<std::uint32_t>(to_byte(glow * style.glow_r / 255.0)) << 16) |
                  (static_cast<std::uint32_t>(to_byte(glow * style.glow_g / 255.0)) << 8) |
                  static_cast<std::uint32_t>(to_byte(glow * style.glow_b / 255.0));
        }
    }
}

ResizeZone chamfer_resize_zone(int x, int y, int width, int height, int cut, int grip, int reach) {
    if (width <= 0 || height <= 0 || grip < 0) return ResizeZone::None;
    if (x < -grip || y < -grip || x >= width + grip || y >= height + grip) return ResizeZone::None;
    if (chamfer_contains_pixel(x, y, width, height, cut)) return ResizeZone::None;
    // 角的范围至少盖住切角缺口,窗口太小时不让两个角重叠。
    cut = clamp_chamfer_cut(width, height, cut);
    const int corner_x = std::min(std::max(reach, cut), width / 2);
    const int corner_y = std::min(std::max(reach, cut), height / 2);
    const bool left = x < corner_x;
    const bool right = x >= width - corner_x;
    const bool top = y < corner_y;
    const bool bottom = y >= height - corner_y;
    if (top && left) return ResizeZone::TopLeft;
    if (top && right) return ResizeZone::TopRight;
    if (bottom && left) return ResizeZone::BottomLeft;
    if (bottom && right) return ResizeZone::BottomRight;
    if (y < 0) return ResizeZone::Top;
    if (y >= height) return ResizeZone::Bottom;
    if (x < 0) return ResizeZone::Left;
    if (x >= width) return ResizeZone::Right;
    return ResizeZone::None;
}

void render_chamfer_grip_mask(std::uint32_t* pixels, const ShadowStrip& strip,
                              int width, int height, int cut, int grip, int reach) {
    if (!pixels || strip.width <= 0 || strip.height <= 0) return;
    for (int row = 0; row < strip.height; ++row) {
        for (int column = 0; column < strip.width; ++column) {
            const bool hit = chamfer_resize_zone(strip.x + column, strip.y + row, width, height, cut, grip,
                                                 reach) != ResizeZone::None;
            pixels[static_cast<std::size_t>(row) * strip.width + column] = hit ? 0x01000000u : 0u;
        }
    }
}

std::optional<WindowShapeRequest> parse_window_shape_request(std::string_view json) {
    nlohmann::json args;
    try {
        args = nlohmann::json::parse(json);
    } catch (...) {
        return std::nullopt;
    }
    if (!args.is_array() || args.empty() || !args[0].is_object()) return std::nullopt;
    const auto& payload = args[0];
    const auto shape = payload.find("shape");
    if (shape == payload.end() || !shape->is_string()) return std::nullopt;
    WindowShapeRequest request;
    const auto name = shape->get<std::string>();
    if (name == "chamfer") {
        request.chamfer = true;
    } else if (name != "none") {
        return std::nullopt;
    }
    const auto glow = payload.find("glow");
    if (glow != payload.end() && !glow->is_null()) {
        if (!glow->is_string()) return std::nullopt;
        const auto color = parse_window_background_color(glow->get<std::string>());
        if (!color) return std::nullopt;
        request.glow_rgb = (static_cast<std::uint32_t>(color->r) << 16) |
                           (static_cast<std::uint32_t>(color->g) << 8) | color->b;
    }
    return request;
}

} // namespace acecode::desktop
