#include "installer_canvas_win.hpp"

#ifdef _WIN32

#include <algorithm>
#include <cmath>

namespace acecode::installer::ui {
namespace {

using Gdiplus::Color;
using Gdiplus::GraphicsPath;
using Gdiplus::RectF;

// 卡片配色取自图标的深蓝端(#082A52),顶部略亮、底部更深;光晕用图标中段的 #1687DA。
constexpr float kTopR = 14.0f, kTopG = 30.0f, kTopB = 52.0f;
constexpr float kBottomR = 8.0f, kBottomG = 17.0f, kBottomB = 31.0f;
constexpr float kHaloR = 22.0f, kHaloG = 135.0f, kHaloB = 218.0f;
constexpr float kHaloStrength = 0.20f;
constexpr float kShadowAlpha = 0.42f;
constexpr float kBorderAlpha = 0.09f;

float rounded_rect_distance(float x, float y, const RectF& rect, float radius) {
    const float cx = rect.X + rect.Width * 0.5f;
    const float cy = rect.Y + rect.Height * 0.5f;
    const float qx = std::fabs(x - cx) - (rect.Width * 0.5f - radius);
    const float qy = std::fabs(y - cy) - (rect.Height * 0.5f - radius);
    const float ox = std::max(qx, 0.0f);
    const float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

std::uint32_t pack(float r, float g, float b, float a) {
    const auto channel = [](float v) {
        return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
    };
    return (channel(a * 255.0f) << 24u) | (channel(r * a) << 16u) | (channel(g * a) << 8u) |
           channel(b * a);
}

void add_rounded(GraphicsPath& path, const RectF& rect, float radius) {
    const float r = std::max(0.5f, std::min(radius, std::min(rect.Width, rect.Height) * 0.5f));
    const float d = r * 2.0f;
    path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

}  // namespace

Surface::~Surface() { release(); }

void Surface::release() {
    if (dc_ && previous_) ::SelectObject(dc_, previous_);
    if (bitmap_) ::DeleteObject(bitmap_);
    if (dc_) ::DeleteDC(dc_);
    dc_ = nullptr;
    bitmap_ = nullptr;
    previous_ = nullptr;
    bits_ = nullptr;
    width_ = height_ = 0;
}

bool Surface::resize(int width, int height) {
    release();
    HDC screen = ::GetDC(nullptr);
    if (!screen) return false;
    dc_ = ::CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    bitmap_ = dc_ ? ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0)
                  : nullptr;
    ::ReleaseDC(nullptr, screen);
    if (!bitmap_ || !pixels) {
        release();
        return false;
    }
    previous_ = ::SelectObject(dc_, bitmap_);
    bits_ = static_cast<std::uint32_t*>(pixels);
    width_ = width;
    height_ = height;
    return true;
}

std::vector<std::uint32_t> render_card_background(int width, int height, const CardStyle& style) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height, 0u);
    const float sigma = std::max(1.0f, style.shadow / 2.6f);
    const float shadow_offset = style.shadow * 0.22f;
    for (int y = 0; y < height; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        const float t = std::clamp((py - style.card.Y) / std::max(1.0f, style.card.Height),
                                   0.0f, 1.0f);
        const float base_r = kTopR + (kBottomR - kTopR) * t;
        const float base_g = kTopG + (kBottomG - kTopG) * t;
        const float base_b = kTopB + (kBottomB - kTopB) * t;
        for (int x = 0; x < width; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float d = rounded_rect_distance(px, py, style.card, style.radius);
            const float ds = rounded_rect_distance(px, py - shadow_offset, style.card,
                                                   style.radius);
            const float shadow = ds <= 0.0f ? kShadowAlpha
                                            : kShadowAlpha * std::exp(-(ds * ds) /
                                                                      (2.0f * sigma * sigma));
            const float coverage = std::clamp(0.5f - d, 0.0f, 1.0f);
            float r = base_r;
            float g = base_g;
            float b = base_b;
            const float hx = px - style.halo_center.X;
            const float hy = py - style.halo_center.Y;
            const float halo = std::max(
                0.0f, 1.0f - std::sqrt(hx * hx + hy * hy) / std::max(1.0f, style.halo_radius));
            const float glow = kHaloStrength * halo * halo;
            r += (kHaloR - r) * glow;
            g += (kHaloG - g) * glow;
            b += (kHaloB - b) * glow;
            const float border = std::clamp(1.0f - std::fabs(d + 0.75f), 0.0f, 1.0f) * kBorderAlpha;
            r += (255.0f - r) * border;
            g += (255.0f - g) * border;
            b += (255.0f - b) * border;
            // 卡片盖在阴影上:预乘后按覆盖率混合。
            const float out_a = coverage + shadow * (1.0f - coverage);
            if (out_a <= 0.0f) continue;
            const float mix_r = (r * coverage) / out_a;
            const float mix_g = (g * coverage) / out_a;
            const float mix_b = (b * coverage) / out_a;
            pixels[static_cast<std::size_t>(y) * width + x] = pack(mix_r, mix_g, mix_b, out_a);
        }
    }
    return pixels;
}

Fonts make_fonts(float scale) {
    Fonts fonts;
    fonts.family = std::make_unique<Gdiplus::FontFamily>(L"Microsoft YaHei UI");
    if (fonts.family->GetLastStatus() != Gdiplus::Ok) {
        fonts.family = std::make_unique<Gdiplus::FontFamily>(L"Segoe UI");
    }
    const auto font = [&fonts, scale](float size, INT style) {
        return std::make_unique<Gdiplus::Font>(fonts.family.get(), size * scale, style,
                                               Gdiplus::UnitPixel);
    };
    fonts.title = font(21.0f, Gdiplus::FontStyleBold);
    fonts.subtitle = font(12.5f, Gdiplus::FontStyleRegular);
    fonts.body = font(13.5f, Gdiplus::FontStyleRegular);
    fonts.caption = font(12.0f, Gdiplus::FontStyleRegular);
    fonts.button = font(14.0f, Gdiplus::FontStyleBold);
    return fonts;
}

Color with_alpha(Color color, float opacity) {
    const float a = std::clamp(opacity, 0.0f, 1.0f) * static_cast<float>(color.GetA());
    return Color(static_cast<BYTE>(a + 0.5f), color.GetR(), color.GetG(), color.GetB());
}

Painter::Painter(std::uint32_t* bits, int width, int height)
    : bitmap_(std::make_unique<Gdiplus::Bitmap>(width, height, width * 4, PixelFormat32bppPARGB,
                                                reinterpret_cast<BYTE*>(bits))),
      graphics_(std::make_unique<Gdiplus::Graphics>(bitmap_.get())) {
    graphics_->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics_->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics_->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics_->SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
}

void Painter::fill_round(const RectF& rect, float radius, Color color) {
    if (color.GetA() == 0) return;
    GraphicsPath path;
    add_rounded(path, rect, radius);
    Gdiplus::SolidBrush brush(color);
    graphics_->FillPath(&brush, &path);
}

void Painter::fill_round_gradient(const RectF& rect, float radius, Color left, Color right) {
    GraphicsPath path;
    add_rounded(path, rect, radius);
    RectF bounds(rect.X - 1.0f, rect.Y, rect.Width + 2.0f, rect.Height);
    Gdiplus::LinearGradientBrush brush(bounds, left, right, Gdiplus::LinearGradientModeHorizontal);
    graphics_->FillPath(&brush, &path);
}

void Painter::stroke_round(const RectF& rect, float radius, Color color, float width) {
    if (color.GetA() == 0) return;
    GraphicsPath path;
    const float inset = width * 0.5f;
    add_rounded(path, RectF(rect.X + inset, rect.Y + inset, rect.Width - width,
                            rect.Height - width),
                radius - inset);
    Gdiplus::Pen pen(color, width);
    graphics_->DrawPath(&pen, &path);
}

void Painter::text(const std::wstring& value, const RectF& rect, const Gdiplus::Font& font,
                   Color color, Align align, bool wrap, bool path_ellipsis) {
    if (value.empty() || color.GetA() == 0) return;
    Gdiplus::StringFormat format;
    format.SetAlignment(align == Align::start    ? Gdiplus::StringAlignmentNear
                        : align == Align::center ? Gdiplus::StringAlignmentCenter
                                                 : Gdiplus::StringAlignmentFar);
    format.SetLineAlignment(wrap ? Gdiplus::StringAlignmentNear : Gdiplus::StringAlignmentCenter);
    if (!wrap) format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(path_ellipsis ? Gdiplus::StringTrimmingEllipsisPath
                                     : Gdiplus::StringTrimmingEllipsisCharacter);
    Gdiplus::SolidBrush brush(color);
    graphics_->DrawString(value.c_str(), static_cast<INT>(value.size()), &font, rect, &format,
                          &brush);
}

float Painter::text_width(const std::wstring& value, const Gdiplus::Font& font) {
    if (value.empty()) return 0.0f;
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(Gdiplus::StringFormatFlagsMeasureTrailingSpaces |
                          Gdiplus::StringFormatFlagsNoWrap);
    RectF bounds;
    graphics_->MeasureString(value.c_str(), static_cast<INT>(value.size()), &font,
                             Gdiplus::PointF(0.0f, 0.0f), &format, &bounds);
    return bounds.Width;
}

void Painter::line(Gdiplus::PointF from, Gdiplus::PointF to, Color color, float width) {
    Gdiplus::Pen pen(color, width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    graphics_->DrawLine(&pen, from, to);
}

void Painter::fill_ellipse(const RectF& rect, Color color) {
    Gdiplus::SolidBrush brush(color);
    graphics_->FillEllipse(&brush, rect);
}

}  // namespace acecode::installer::ui

#endif
