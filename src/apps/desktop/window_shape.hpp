#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace acecode::desktop {

// ── 主题异形窗口(切角)────────────────────────────────────────────────
// 主题包声明 "window": {"shape": "chamfer"} 时,Windows 桌面壳把窗口四个角按
// 45° 切掉(SetWindowRgn),与 HUD 外框画在页面里的斜边重合。切掉区域后系统
// 阴影会整圈消失,所以另用几块鼠标可穿透的分层窗口按同一轮廓画阴影。这里是
// 两者共用的纯几何:区域按行给出、阴影按同一个「像素是否在窗口内」的判定
// 渲染,两边逐像素互补,斜边处不会出现漏光或重叠。

// 与 HUD 外框 CSS 的切角(--c: 16px)一致,单位是 96 DPI 下的像素。
inline constexpr int kChamferCutDip = 16;
// 阴影向外延伸的宽度与整体下移量(96 DPI 像素)。
inline constexpr int kChamferShadowMarginDip = 14;
inline constexpr int kChamferShadowOffsetDip = 1;

int chamfer_cut_px(int cut_dip, int dpi);

// 窗口太小时切角收窄,保证上下两段切角不重叠。
int clamp_chamfer_cut(int width, int height, int cut);

// 第 y 行(0 起)左右各让出多少像素。规则按像素中心:中心落在斜边内侧
// (含斜边上)的像素属于窗口,所以第 0 行让出 cut-1,第 cut-1 行让出 0。
int chamfer_row_inset(int y, int height, int cut);

struct ShapeRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool operator==(const ShapeRect& other) const {
        return left == other.left && top == other.top && right == other.right && bottom == other.bottom;
    }
};

// 组成切角区域的矩形(窗口坐标):上、下切角每行一条,中间一整块。
std::vector<ShapeRect> chamfer_region_rects(int width, int height, int cut);

bool chamfer_contains_pixel(int x, int y, int width, int height, int cut);

// 点到切角八边形轮廓的距离,点在形内返回 0。坐标为连续值(像素中心 = x + 0.5)。
double chamfer_outside_distance(double x, double y, int width, int height, int cut);

// 阴影窗口:上、下两条横跨整个宽度并向内覆盖切角高度,左右两条夹在中间,
// 坐标相对主窗口左上角。上下条向内多出的部分在形内,渲染为全透明。
struct ShadowStrip {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const ShadowStrip& other) const {
        return x == other.x && y == other.y && width == other.width && height == other.height;
    }
};

std::vector<ShadowStrip> chamfer_shadow_strips(int width, int height, int cut, int margin);

struct ShadowStyle {
    int margin = 0;
    int offset_y = 0;
    bool active = true;
    // 贴着轮廓的一圈主题色辉光;glow_width 为 0 时只有黑色阴影。
    std::uint8_t glow_r = 0;
    std::uint8_t glow_g = 0;
    std::uint8_t glow_b = 0;
    int glow_width = 0;
};

// 预乘的 BGRA 像素,写入 strip.width * strip.height 个 uint32(0xAARRGGBB)。
void render_chamfer_shadow(std::uint32_t* pixels, const ShadowStrip& strip,
                           int width, int height, int cut, const ShadowStyle& style);

// ── 窗口外侧的缩放把手 ─────────────────────────────────────────────────
// 切角时窗口没有系统的隐形缩放边,切角缺口也不属于窗口,鼠标在原来的角上会点穿到
// 后面的窗口。所以在窗口外侧另放几块几乎全透明(alpha 1)的分层窗口当把手:贴着
// 窗口矩形向外 grip 像素,外加四个切角缺口;离角 reach 以内算角(沿两条边各延伸
// reach),其余算边。窗口内的点不归这里,由页面的 FramelessResizeHandles 处理。
// 角在页面里的把手同样沿边延伸 28px(globals.css),两边连成一片。
inline constexpr int kChamferCornerReachDip = 28;

enum class ResizeZone { None, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };

ResizeZone chamfer_resize_zone(int x, int y, int width, int height, int cut, int grip, int reach);

// 把手窗口与阴影窗口同样摆放(chamfer_shadow_strips,外伸 grip);像素在把手范围内
// 写 alpha 1 的黑色(肉眼不可见,但分层窗口只让 alpha 为 0 的像素点穿),其余为 0。
void render_chamfer_grip_mask(std::uint32_t* pixels, const ShadowStrip& strip,
                              int width, int height, int cut, int grip, int reach);

struct WindowShapeRequest {
    bool chamfer = false;
    std::optional<std::uint32_t> glow_rgb;  // 0xRRGGBB
};

// 解析 aceDesktop_setWindowShape 的参数(webview bind 传来的 JSON 数组)。
// 只认 [{"shape":"chamfer"|"none","glow":"#rrggbb"?}],其它一律 nullopt。
std::optional<WindowShapeRequest> parse_window_shape_request(std::string_view json);

} // namespace acecode::desktop
