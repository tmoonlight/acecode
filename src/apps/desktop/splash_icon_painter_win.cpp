#include "splash_icon_painter_win.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace acecode::desktop {
namespace {

using Gdiplus::Color;
using Gdiplus::GraphicsPath;
using Gdiplus::PointF;

struct GradientStop {
    float offset;
    BYTE r, g, b;
};

// 方砖、'A'、'>' 的渐变,逐字取自 assets/branding/acecode-icon.svg。
constexpr std::array<GradientStop, 4> kTileStops{{
    {0.0f, 0x38, 0xD5, 0xF7},
    {0.38f, 0x16, 0x87, 0xDA},
    {0.7f, 0x25, 0x63, 0xEB},
    {1.0f, 0x08, 0x2A, 0x52},
}};
constexpr std::array<GradientStop, 2> kLetterStops{{
    {0.0f, 0xFF, 0xFF, 0xFF},
    {1.0f, 0xE0, 0xF2, 0xFE},
}};
constexpr std::array<GradientStop, 2> kPromptStops{{
    {0.0f, 0xCF, 0xFA, 0xFE},
    {1.0f, 0x67, 0xE8, 0xF9},
}};
constexpr BYTE kGlowR = 0x67, kGlowG = 0xE8, kGlowB = 0xF9;

constexpr float kChevronStroke = 38.0f;
// 波浪最高点约在 y=330;把它推到画布底边以下,整段波浪就都在方砖外。
constexpr double kWaveCrestY = 330.0;

BYTE alpha_of(double opacity) {
    return static_cast<BYTE>(std::clamp(opacity, 0.0, 1.0) * 255.0 + 0.5);
}

// SVG 渐变在端点外取端点色;GDI+ 只能平铺,所以把渐变线两端各延长一半、端点色铺满延长段。
template <std::size_t N>
std::unique_ptr<Gdiplus::LinearGradientBrush> padded_gradient(
    PointF from, PointF to, const std::array<GradientStop, N>& stops, double opacity) {
    constexpr float kPad = 0.5f;
    const float dx = to.X - from.X;
    const float dy = to.Y - from.Y;
    const PointF start(from.X - dx * kPad, from.Y - dy * kPad);
    const PointF end(to.X + dx * kPad, to.Y + dy * kPad);
    const BYTE alpha = alpha_of(opacity);
    std::array<Color, N + 2> colors{};
    std::array<float, N + 2> positions{};
    colors[0] = Color(alpha, stops.front().r, stops.front().g, stops.front().b);
    positions[0] = 0.0f;
    for (std::size_t i = 0; i < N; ++i) {
        colors[i + 1] = Color(alpha, stops[i].r, stops[i].g, stops[i].b);
        positions[i + 1] = (stops[i].offset + kPad) / (1.0f + 2.0f * kPad);
    }
    colors[N + 1] = Color(alpha, stops.back().r, stops.back().g, stops.back().b);
    positions[N + 1] = 1.0f;
    auto brush = std::make_unique<Gdiplus::LinearGradientBrush>(
        start, end, colors.front(), colors.back());
    brush->SetInterpolationColors(colors.data(), positions.data(), static_cast<INT>(N + 2));
    brush->SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    return brush;
}

void add_tile(GraphicsPath& path) {
    const float x = static_cast<float>(splash_anim::kTileX);
    const float y = static_cast<float>(splash_anim::kTileY);
    const float s = static_cast<float>(splash_anim::kTileSize);
    const float d = static_cast<float>(splash_anim::kTileRadius * 2.0);
    path.StartFigure();
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + s - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + s - d, y + s - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + s - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// 几何字母 A(evenodd),路径取自 acecode-icon.svg。
void add_letter(GraphicsPath& path) {
    path.StartFigure();
    path.AddLine(73.0f, 384.0f, 164.5f, 139.5f);
    path.AddBezier(164.5f, 139.5f, 169.1f, 127.2f, 178.3f, 121.0f, 192.0f, 121.0f);
    path.AddLine(192.0f, 121.0f, 210.0f, 121.0f);
    path.AddBezier(210.0f, 121.0f, 223.7f, 121.0f, 232.9f, 127.2f, 237.5f, 139.5f);
    path.AddLine(237.5f, 139.5f, 329.0f, 384.0f);
    path.AddLine(329.0f, 384.0f, 265.0f, 384.0f);
    path.AddLine(265.0f, 384.0f, 245.7f, 326.0f);
    path.AddLine(245.7f, 326.0f, 154.3f, 326.0f);
    path.AddLine(154.3f, 326.0f, 135.0f, 384.0f);
    path.CloseFigure();
    path.StartFigure();
    path.AddLine(174.7f, 266.0f, 225.3f, 266.0f);
    path.AddLine(225.3f, 266.0f, 200.3f, 190.8f);
    path.CloseFigure();
}

std::vector<PointF> wave_polygon(const splash_anim::Frame& frame) {
    constexpr int kSamples = 64;
    const double left = splash_anim::kTileX;
    const double right = splash_anim::kTileX + splash_anim::kTileSize;
    // 升起时整段波浪从方砖底边下方往上推(左右平移会在波浪端头留一条竖直硬边)。
    // wave_rise 大于 1 是安装程序的「水位上涨」:波面继续上移,水体仍铺到方砖底边。
    const double dy = (512.0 - kWaveCrestY) * (1.0 - frame.wave_rise);
    const float bottom = 512.0f + static_cast<float>(std::max(0.0, dy));
    std::vector<PointF> points;
    points.reserve(kSamples + 3);
    for (int i = 0; i <= kSamples; ++i) {
        const double x = left + (right - left) * i / kSamples;
        points.emplace_back(static_cast<float>(x),
                            static_cast<float>(splash_anim::wave_y(
                                x, frame.wave_phase, frame.wave_morph) + dy));
    }
    points.emplace_back(static_cast<float>(right), bottom);
    points.emplace_back(static_cast<float>(left), bottom);
    return points;
}

// '>' 已描出的那一段折线(随 chevron_offset 向右平移)。
std::vector<PointF> chevron_polyline(const splash_anim::Frame& frame) {
    std::vector<PointF> points;
    if (frame.chevron_draw <= 0.001) return points;
    const float dx = static_cast<float>(frame.chevron_offset);
    points.emplace_back(348.0f + dx, 203.0f);
    if (frame.chevron_draw > 0.5) points.emplace_back(431.0f + dx, 256.0f);
    const auto tip = splash_anim::chevron_point_at(frame.chevron_draw);
    points.emplace_back(static_cast<float>(tip[0]) + dx, static_cast<float>(tip[1]));
    return points;
}

void paint_chevron_glow(Gdiplus::Graphics& g,
                        const splash_anim::Frame& frame,
                        const std::vector<PointF>& chevron) {
    if (frame.chevron_glow <= 0.0) return;
    // 以 '>' 外接框中心为心的径向柔光,对应宣传片落地时的径向闪光。
    const float cx = 394.0f + static_cast<float>(frame.chevron_offset);
    const float radius = 150.0f;
    GraphicsPath halo;
    halo.AddEllipse(cx - radius, 256.0f - radius, radius * 2.0f, radius * 2.0f);
    Gdiplus::PathGradientBrush radial(&halo);
    radial.SetCenterPoint(PointF(cx, 256.0f));
    radial.SetCenterColor(Color(alpha_of(0.5 * frame.chevron_glow), kGlowR, kGlowG, kGlowB));
    Color edge(0, kGlowR, kGlowG, kGlowB);
    INT edge_count = 1;
    radial.SetSurroundColors(&edge, &edge_count);
    g.FillPath(&radial, &halo);

    // 描边外再叠几层逐渐变宽、变淡的笔画,近似模糊辉光。
    if (chevron.size() < 2) return;
    for (int layer = 1; layer <= 4; ++layer) {
        Gdiplus::Pen pen(Color(alpha_of(frame.chevron_glow * 0.22 / layer), kGlowR, kGlowG, kGlowB),
                         kChevronStroke + 14.0f * layer);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLines(&pen, chevron.data(), static_cast<INT>(chevron.size()));
    }
}

}  // namespace

SplashGraphicsRuntime::SplashGraphicsRuntime() {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) token_ = token;
}

SplashGraphicsRuntime::~SplashGraphicsRuntime() {
    if (token_) Gdiplus::GdiplusShutdown(static_cast<ULONG_PTR>(token_));
}

bool paint_splash_icon(std::uint32_t* pixels,
                       int width,
                       int height,
                       double origin_x,
                       double origin_y,
                       double size,
                       const splash_anim::Frame& frame) {
    if (!pixels || width <= 0 || height <= 0 || size <= 0.0) return false;
    Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppPARGB,
                           reinterpret_cast<BYTE*>(pixels));
    Gdiplus::Graphics g(&bitmap);
    if (g.GetLastStatus() != Gdiplus::Ok) return false;
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

    // 视口 → 像素:定位、震动、以砖心缩放。
    const float scale = static_cast<float>(size / 512.0);
    Gdiplus::Matrix transform(scale, 0.0f, 0.0f, scale,
                              static_cast<float>(origin_x), static_cast<float>(origin_y));
    transform.Translate(static_cast<float>(frame.shake_x), static_cast<float>(frame.shake_y));
    const float center = static_cast<float>(splash_anim::kTileCenter);
    transform.Translate(center, center);
    transform.Scale(static_cast<float>(frame.tile_scale), static_cast<float>(frame.tile_scale));
    transform.Translate(-center, -center);
    g.SetTransform(&transform);

    GraphicsPath tile;
    add_tile(tile);
    {
        auto brush = padded_gradient(
            PointF(74.0f, 38.0f), PointF(438.0f, 476.0f), kTileStops, frame.tile_opacity);
        g.FillPath(brush.get(), &tile);
    }

    // 波浪:#020617(定格时 12%),裁在圆角砖内。
    g.SetClip(&tile);
    if (frame.wave_rise > 0.0) {
        const auto wave = wave_polygon(frame);
        Gdiplus::SolidBrush ink(Color(alpha_of(frame.wave_opacity * frame.tile_opacity), 0x02, 0x06, 0x17));
        g.FillPolygon(&ink, wave.data(), static_cast<INT>(wave.size()));
    }
    g.ResetClip();

    if (frame.a_reveal > 0.0) {
        GraphicsPath letter(Gdiplus::FillModeAlternate);
        add_letter(letter);
        if (frame.a_reveal < 1.0) {
            const float top = static_cast<float>(splash_anim::a_clip_top(frame.a_reveal));
            g.SetClip(Gdiplus::RectF(0.0f, top, 512.0f, 512.0f - top));
        }
        auto brush = padded_gradient(
            PointF(92.0f, 126.0f), PointF(314.0f, 388.0f), kLetterStops, frame.tile_opacity);
        g.FillPath(brush.get(), &letter);
        g.ResetClip();
    }

    if (frame.heat > 0.0) {
        Gdiplus::SolidBrush white(Color(alpha_of(frame.heat * 0.75), 0xFF, 0xFF, 0xFF));
        g.FillPath(&white, &tile);
    }

    const auto chevron = chevron_polyline(frame);
    paint_chevron_glow(g, frame, chevron);
    if (chevron.size() >= 2) {
        const float dx = static_cast<float>(frame.chevron_offset);
        auto brush = padded_gradient(
            PointF(345.0f + dx, 199.0f), PointF(432.0f + dx, 313.0f), kPromptStops,
            frame.tile_opacity);
        Gdiplus::Pen pen(brush.get(), kChevronStroke);
        pen.SetStartCap(Gdiplus::LineCapSquare);
        pen.SetEndCap(Gdiplus::LineCapSquare);
        pen.SetLineJoin(Gdiplus::LineJoinMiter);
        pen.SetMiterLimit(4.0f);
        g.DrawLines(&pen, chevron.data(), static_cast<INT>(chevron.size()));
    }
    g.Flush(Gdiplus::FlushIntentionSync);
    return true;
}

}  // namespace acecode::desktop

#endif
