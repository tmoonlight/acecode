#include "web_host_chrome_win.hpp"

#ifdef _WIN32
#include "window_chrome.hpp"
#include <winternl.h>
#include <algorithm>

namespace acecode::desktop::detail {

int dpi_scale(int value, UINT dpi) {
    return static_cast<int>((static_cast<long long>(value) * static_cast<long long>(dpi)) / 96);
}

// ── Win10 顶边 1px 边框线 ─────────────────────────────────────────────
// 系统在 Win10 上不画无标题栏窗口的顶边线(原因见 window_chrome.hpp)。非最大化
// 时把主 WebView 下移 inset 像素,宿主窗口在露出的这一行按系统配色自己画线。
// 不走 DwmExtendFrameIntoClientArea 让系统画:Win10 上那样激活时四边边框都会
// 变白(microsoft/terminal#4577)。不画在网页里:页面拿不到激活态 / 主题色,
// 非整数缩放下 CSS 1px 也不等于 1 个物理像素(VS Code 因此在 Windows 上停用了
// CSS 窗口边框)。这一行归宿主窗口,frameless_hit_test 会给它原生 HTTOP。

// 宿主窗口按激活态画线;由 WM_NCACTIVATE 维护,与系统画左/右/下边框同源。
bool g_top_border_active = false;
// 线下方的标题栏底色 = 前端推来的 --ace-bg(apply_window_background 同步)。
WindowBackgroundColor g_top_border_background = kDefaultWindowBackground;

std::uint32_t windows_build_number() {
    static const std::uint32_t build = [] {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return std::uint32_t{0};
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        auto get_version = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (!get_version || get_version(&info) != 0) return std::uint32_t{0};
        return static_cast<std::uint32_t>(info.dwBuildNumber);
    }();
    return build;
}

// 主题切角窗口(主题包 "window": {"shape": "chamfer"})。只由前端在当前主题声明
// 切角时经 aceDesktop_setWindowShape 打开;没打开时下面所有分支都不走,窗口行为
// 与普通主题完全一样。打开后:非最大化时去掉左/右/下的隐形缩放边(客户区 = 整个
// 窗口,否则系统把区域外的缩放边也算进可见边界,贴边分屏会留缝),按切角设置
// 窗口区域,系统阴影随之消失,由 g_chamfer_shadow 的分层窗口补画;缩放靠页面
// 四边四角的拖拽条(FramelessResizeHandles)。最大化时恢复矩形、隐藏阴影。
WindowShapeState g_window_shape;
ChamferShadowWindows g_chamfer_shadow;

int top_border_inset(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) return 0;
    if (g_window_shape.chamfer) return 0;
    SelfDrawnTopBorderLayoutInput input;
    input.supported = windows_needs_self_drawn_top_border(windows_build_number());
    if (!input.supported) return 0;
    input.has_resize_frame =
        (::GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) != 0;
    input.maximized = ::IsZoomed(hwnd) != FALSE;
    input.minimized = ::IsIconic(hwnd) != FALSE;
    input.system_dpi = static_cast<int>(::GetDpiForSystem());
    return self_drawn_top_border_inset(input);
}

std::optional<std::uint32_t> read_dwm_dword(const wchar_t* name) {
    DWORD value = 0;
    DWORD value_size = sizeof(value);
    const LONG result = ::RegGetValueW(HKEY_CURRENT_USER,
                                       L"Software\\Microsoft\\Windows\\DWM",
                                       name,
                                       RRF_RT_REG_DWORD,
                                       nullptr,
                                       &value,
                                       &value_size);
    if (result != ERROR_SUCCESS || value_size != sizeof(value)) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

// 每次绘制现读注册表:只在这 1px 进入重绘区时才走到,开销可忽略;用户改了
// 主题色设置但没有广播到本窗口时,下一次重绘也能自行纠正。
COLORREF top_border_colorref() {
    SelfDrawnTopBorderColorInput input;
    input.active = g_top_border_active;
    input.windows_build = windows_build_number();
    const auto prevalence = read_dwm_dword(L"ColorPrevalence");
    input.accent_on_borders = prevalence && *prevalence == 1;
    if (input.active && input.accent_on_borders) {
        input.colorization_color = read_dwm_dword(L"ColorizationColor");
        input.colorization_balance = read_dwm_dword(L"ColorizationColorBalance");
    }
    input.background = RgbColor{g_top_border_background.r,
                                g_top_border_background.g,
                                g_top_border_background.b};
    const RgbColor color = self_drawn_top_border_color(input);
    return RGB(color.r, color.g, color.b);
}

void invalidate_top_border(HWND hwnd) {
    const int inset = top_border_inset(hwnd);
    if (inset <= 0) return;
    RECT client{};
    if (!::GetClientRect(hwnd, &client)) return;
    const RECT strip{0, 0, client.right, inset};
    ::InvalidateRect(hwnd, &strip, FALSE);
}

// 返回 false 表示当前不画线(Win11 / 最大化等),调用方走默认 WM_PAINT。
bool paint_top_border(HWND hwnd) {
    const int inset = top_border_inset(hwnd);
    if (inset <= 0) return false;
    PAINTSTRUCT paint{};
    HDC dc = ::BeginPaint(hwnd, &paint);
    if (dc) {
        RECT client{};
        ::GetClientRect(hwnd, &client);
        const RECT strip{0, 0, client.right, inset};
        RECT dirty{};
        if (::IntersectRect(&dirty, &strip, &paint.rcPaint)) {
            if (HBRUSH brush = ::CreateSolidBrush(top_border_colorref())) {
                ::FillRect(dc, &dirty, brush);
                ::DeleteObject(brush);
            }
        }
    }
    ::EndPaint(hwnd, &paint);
    return true;
}

void resize_webview_widget(HWND hwnd) {
    HWND widget = ::FindWindowExW(hwnd, nullptr, kWebViewWidgetClassName, nullptr);
    if (!widget) return;

    RECT client{};
    if (!::GetClientRect(hwnd, &client)) return;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int inset = std::clamp(top_border_inset(hwnd), 0, std::max(0, height));
    ::MoveWindow(widget, 0, inset, width, height - inset, TRUE);
    if (inset > 0) invalidate_top_border(hwnd);
}

int window_shape_cut(HWND hwnd) {
    return chamfer_cut_px(kChamferCutDip, static_cast<int>(::GetDpiForWindow(hwnd)));
}

void apply_window_shape_region(HWND hwnd) {
    auto& shape = g_window_shape;
    if (!shape.chamfer || ::IsZoomed(hwnd)) {
        if (shape.region_set) {
            ::SetWindowRgn(hwnd, nullptr, TRUE);
            shape.region_set = false;
        }
        return;
    }
    if (::IsIconic(hwnd)) return;
    RECT rect{};
    if (!::GetWindowRect(hwnd, &rect)) return;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int cut = window_shape_cut(hwnd);
    if (shape.region_set && width == shape.region_width && height == shape.region_height &&
        cut == shape.region_cut) {
        return;
    }
    HRGN region = ::CreateRectRgn(0, 0, 0, 0);
    if (!region) return;
    for (const auto& rect_part : chamfer_region_rects(width, height, cut)) {
        if (HRGN part = ::CreateRectRgn(rect_part.left, rect_part.top, rect_part.right, rect_part.bottom)) {
            ::CombineRgn(region, region, part, RGN_OR);
            ::DeleteObject(part);
        }
    }
    // SetWindowRgn takes ownership only on success.
    if (!::SetWindowRgn(hwnd, region, TRUE)) {
        ::DeleteObject(region);
        return;
    }
    shape.region_set = true;
    shape.region_width = width;
    shape.region_height = height;
    shape.region_cut = cut;
}

ShadowStyle window_shadow_style(HWND hwnd) {
    const UINT dpi = ::GetDpiForWindow(hwnd);
    ShadowStyle style;
    style.margin = std::max(1, dpi_scale(kChamferShadowMarginDip, dpi));
    style.offset_y = std::max(1, dpi_scale(kChamferShadowOffsetDip, dpi));
    style.active = g_top_border_active;
    if (g_window_shape.glow_rgb) {
        const std::uint32_t rgb = *g_window_shape.glow_rgb;
        style.glow_r = static_cast<std::uint8_t>((rgb >> 16) & 0xFF);
        style.glow_g = static_cast<std::uint8_t>((rgb >> 8) & 0xFF);
        style.glow_b = static_cast<std::uint8_t>(rgb & 0xFF);
        style.glow_width = std::max(1, dpi_scale(3, dpi));
    }
    return style;
}

void update_window_shape_shadow(HWND hwnd) {
    if (g_window_shape.chamfer && ::IsWindowVisible(hwnd) && !::IsIconic(hwnd) && !::IsZoomed(hwnd)) {
        // 外侧把手与切角时去掉的隐形缩放边一样厚,角上沿边延伸到与页面里的角把手相接。
        const UINT dpi = ::GetDpiForWindow(hwnd);
        const int grip = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
                         ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        g_chamfer_shadow.update(hwnd, window_shape_cut(hwnd), window_shadow_style(hwnd), grip,
                                dpi_scale(kChamferCornerReachDip, dpi));
    } else {
        g_chamfer_shadow.hide();
    }
}

void refresh_non_client_frame(HWND hwnd) {
    RECT rect{};
    if (!::GetWindowRect(hwnd, &rect)) return;
    ::SetWindowPos(hwnd,
                   nullptr,
                   rect.left,
                   rect.top,
                   rect.right - rect.left,
                   rect.bottom - rect.top,
                   SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE |
                       SWP_NOSIZE);
}

} // namespace acecode::desktop::detail
#endif
