#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace acecode::desktop {

// webview/webview 库给 WebView2 宿主子窗口注册的窗口类名(webview.h 的
// win32_edge_engine)。web_host.cpp 靠它摆放主 WebView,Agent Browser 靠它
// 换算网页坐标在宿主窗口客户区里的原点。
inline constexpr wchar_t kWebViewWidgetClassName[] = L"webview_widget";

enum class FramelessHitTestArea {
    Client,
    Caption,
    Left,
    Right,
    Top,
    TopLeft,
    TopRight,
    Bottom,
    BottomLeft,
    BottomRight,
};

struct FramelessHitTestInput {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int frame_x = 0;
    int frame_y = 0;
    int padding = 0;
    int drag_height = 0;
    bool maximized = false;
};

int frameless_resize_border(int frame, int padding);
FramelessHitTestArea classify_frameless_hit_test(const FramelessHitTestInput& input);

// 把 JS 端传的 direction 字符串映射到 resize 边/角枚举。
// 接受 "top"/"bottom"/"left"/"right" 四边 + "top-left"/"top-right"/
// "bottom-left"/"bottom-right" 四角。其它(包括 "client"/"caption"/空串/
// 大小写不匹配)返回 nullopt — caller 应当拒绝调用,不要默默 fallback,
// 否则前端打错字时会出现"以为在 resize 顶,实际却开始拖动"的诡异行为。
std::optional<FramelessHitTestArea> parse_resize_direction(std::string_view direction);

// ── Win10 顶边 1px 边框线 ─────────────────────────────────────────────
// Win10 的 1px 窗口边框由系统画在四周的缩放边(非客户区)上。无标题栏窗口的
// 顶边非客户区必须为 0(否则系统会画回原生标题栏),顶上那根线系统就不画了,
// 由宿主窗口自己补,做法同 Chromium BrowserFrameViewWin::PaintTitlebar。
// Win11 的边框盖在整个窗口上画,不需要补。

struct RgbColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    bool operator==(const RgbColor& other) const {
        return r == other.r && g == other.g && b == other.b;
    }
};

// build 是 RtlGetVersion 读到的真实 build 号,0 表示未知。只有 Win10
// (10240 <= build < 22000)需要补画。
bool windows_needs_self_drawn_top_border(std::uint32_t build);

// 线的粗细(物理像素)= floor(系统 DPI / 96),至少 1。按系统 DPI 而不是窗口
// 所在显示器的 DPI,Win10 自己画边框就是这个口径(Chromium WindowTopY 注释:
// "floor(system dsf) pixels when restored")。
int self_drawn_top_border_thickness(int system_dpi);

struct SelfDrawnTopBorderLayoutInput {
    bool supported = false;         // windows_needs_self_drawn_top_border 的结果
    bool has_resize_frame = false;  // 带 WS_THICKFRAME,左/右/下才有系统边框
    bool maximized = false;
    bool minimized = false;
    int system_dpi = 96;
};

// 主 WebView 顶上要让出的像素数;为 0 时 WebView 铺满客户区、不画线。
int self_drawn_top_border_inset(const SelfDrawnTopBorderLayoutInput& input);

struct SelfDrawnTopBorderColorInput {
    bool active = true;               // WM_NCACTIVATE 给出的激活态
    std::uint32_t windows_build = 0;  // 0 = 未知,按 1809 及以后处理
    bool accent_on_borders = false;   // HKCU\...\DWM\ColorPrevalence == 1
    std::optional<std::uint32_t> colorization_color;    // DWM ColorizationColor
    std::optional<std::uint32_t> colorization_balance;  // DWM ColorizationColorBalance
    RgbColor background;              // 线下方的标题栏底色
};

// 颜色口径照抄 Chromium(BrowserFrameViewWin + AccentColorObserver),与系统画的
// 左/右/下边框一致:
// - 激活且开了「在标题栏和窗口边框上显示主题色」:主题色与 #D9D9D9 按
//   ColorizationColorBalance% 混合(>100 的异常值按 80);
// - 激活且没开:alpha 0xA8 的 #262626 叠在底色上,白底上是 #707070;
//   1809 之前的 Win10 激活边框是白色;
// - 失焦:alpha 0x80 的 #555555 叠在底色上,白底上是 #AAAAAA。
RgbColor self_drawn_top_border_color(const SelfDrawnTopBorderColorInput& input);

} // namespace acecode::desktop