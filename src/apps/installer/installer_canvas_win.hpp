#pragma once

// 安装窗口的绘制底座:分层窗口用的 32 位预乘 BGRA 画布、带柔和阴影的卡片底图、
// 以及按钮、开关、输入框等控件共用的 GDI+ 画法。只在 Windows 上编译。

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acecode::installer::ui {

struct CardStyle {
    Gdiplus::RectF card;
    float radius = 18.0f;
    float shadow = 28.0f;
    Gdiplus::PointF halo_center;
    float halo_radius = 200.0f;
};

// 窗口大小的 DIB 与内存 DC,交给 UpdateLayeredWindow。
class Surface {
public:
    Surface() = default;
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    bool resize(int width, int height);
    std::uint32_t* bits() const { return bits_; }
    int width() const { return width_; }
    int height() const { return height_; }
    HDC dc() const { return dc_; }

private:
    void release();

    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    std::uint32_t* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

// 卡片 + 阴影 + 图标背后的光晕,逐像素算一次后缓存,每帧整块拷贝。
std::vector<std::uint32_t> render_card_background(int width, int height, const CardStyle& style);

enum class Align { start, center, end };

struct Fonts {
    std::unique_ptr<Gdiplus::FontFamily> family;
    std::unique_ptr<Gdiplus::Font> title;
    std::unique_ptr<Gdiplus::Font> subtitle;
    std::unique_ptr<Gdiplus::Font> body;
    std::unique_ptr<Gdiplus::Font> caption;
    std::unique_ptr<Gdiplus::Font> button;
};
Fonts make_fonts(float scale);

Gdiplus::Color with_alpha(Gdiplus::Color color, float opacity);

class Painter {
public:
    Painter(std::uint32_t* bits, int width, int height);
    Gdiplus::Graphics& g() { return *graphics_; }

    void fill_round(const Gdiplus::RectF& rect, float radius, Gdiplus::Color color);
    void fill_round_gradient(const Gdiplus::RectF& rect, float radius,
                             Gdiplus::Color left, Gdiplus::Color right);
    void stroke_round(const Gdiplus::RectF& rect, float radius, Gdiplus::Color color,
                      float width);
    void text(const std::wstring& value, const Gdiplus::RectF& rect, const Gdiplus::Font& font,
              Gdiplus::Color color, Align align, bool wrap = false, bool path_ellipsis = false);
    float text_width(const std::wstring& value, const Gdiplus::Font& font);
    void line(Gdiplus::PointF from, Gdiplus::PointF to, Gdiplus::Color color, float width);
    void fill_ellipse(const Gdiplus::RectF& rect, Gdiplus::Color color);

private:
    std::unique_ptr<Gdiplus::Bitmap> bitmap_;
    std::unique_ptr<Gdiplus::Graphics> graphics_;
};

}  // namespace acecode::installer::ui

#endif
