#include "tray_unread_icon_win.hpp"

#ifdef _WIN32
#include "utils/logger.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>

namespace acecode::desktop {
namespace {
constexpr wchar_t kAppIconResourceName[] = L"IDI_ICON1";
constexpr int kAppIconResourceId = 1;

HICON load_app_icon_for_tray() {
    // acecode.rc.in 把 acecode.ico 编成 IDI_ICON1。旧构建里 IDI_ICON1 可能是
    // 命名资源,新构建里是数字资源(1),所以两种都试一遍。
    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    int sx = ::GetSystemMetrics(SM_CXSMICON);
    int sy = ::GetSystemMetrics(SM_CYSMICON);
    if (HICON ic = static_cast<HICON>(::LoadImageW(
            hinst, kAppIconResourceName, IMAGE_ICON, sx, sy, LR_DEFAULTCOLOR))) {
        LOG_INFO("[desktop] tray icon: loaded named small frame via LoadImageW " +
                 std::to_string(sx) + "x" + std::to_string(sy));
        return ic;
    }
    if (HICON ic = static_cast<HICON>(::LoadImageW(
            hinst, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON, sx, sy, LR_DEFAULTCOLOR))) {
        LOG_INFO("[desktop] tray icon: loaded small frame via LoadImageW " +
                 std::to_string(sx) + "x" + std::to_string(sy));
        return ic;
    }
    int lx = ::GetSystemMetrics(SM_CXICON);
    int ly = ::GetSystemMetrics(SM_CYICON);
    if (HICON ic = static_cast<HICON>(::LoadImageW(
            hinst, kAppIconResourceName, IMAGE_ICON, lx, ly, LR_DEFAULTCOLOR))) {
        LOG_WARN("[desktop] tray icon: small frame load failed, fell back to "
                 "named large frame (will be downscaled by shell)");
        return ic;
    }
    if (HICON ic = static_cast<HICON>(::LoadImageW(
            hinst, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON, lx, ly, LR_DEFAULTCOLOR))) {
        LOG_WARN("[desktop] tray icon: small frame load failed, fell back to "
                 "large frame (will be downscaled by shell)");
        return ic;
    }
    if (HICON ic = ::LoadIconW(hinst, kAppIconResourceName)) {
        LOG_WARN("[desktop] tray icon: LoadImageW failed, fell back to named LoadIconW");
        return ::CopyIcon(ic);
    }
    if (HICON ic = ::LoadIconW(hinst, MAKEINTRESOURCEW(kAppIconResourceId))) {
        LOG_WARN("[desktop] tray icon: LoadImageW failed, fell back to LoadIconW");
        return ::CopyIcon(ic);
    }
    LOG_ERROR("[desktop] tray icon: app icon resource missing, using IDI_APPLICATION");
    // 32512 = IDI_APPLICATION 数值常量。
    return ::CopyIcon(::LoadIconW(nullptr, MAKEINTRESOURCEW(32512)));
}

struct TrayGraphicsRuntime {
    ULONG_PTR token = 0;
    TrayGraphicsRuntime() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) token = 0;
    }
    ~TrayGraphicsRuntime() { if (token) Gdiplus::GdiplusShutdown(token); }
};
}

UniqueTrayIcon create_tray_unread_icon(HICON original, int size) {
    if (!original || size < 16 || size > 256) return {};
    static TrayGraphicsRuntime runtime;
    if (!runtime.token) return {};
    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppPARGB);
    Gdiplus::Bitmap logo(original);
    if (bitmap.GetLastStatus() != Gdiplus::Ok || logo.GetLastStatus() != Gdiplus::Ok) return {};
    {
        Gdiplus::Graphics graphics(&bitmap);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        if (graphics.DrawImage(&logo, 0, 0, size, size) != Gdiplus::Ok) return {};
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float diameter = size * 0.40f;
        const float border = std::max(1.0f, size * 0.045f);
        Gdiplus::RectF circle(size - diameter, size - diameter, diameter - 0.5f, diameter - 0.5f);
        Gdiplus::SolidBrush outline(Gdiplus::Color(255, 255, 255, 255));
        Gdiplus::SolidBrush red(Gdiplus::Color(255, 230, 48, 58));
        if (graphics.FillEllipse(&outline, circle) != Gdiplus::Ok) return {};
        circle.Inflate(-border, -border);
        if (graphics.FillEllipse(&red, circle) != Gdiplus::Ok) return {};
    }
    HICON icon = nullptr;
    if (bitmap.GetHICON(&icon) != Gdiplus::Ok) return {};
    return UniqueTrayIcon(icon);
}

bool TrayUnreadIconState::refresh(NOTIFYICONDATAW& notification, bool unread, bool add) {
    UniqueTrayIcon original(load_app_icon_for_tray());
    if (!original) return false;
    UniqueTrayIcon marked;
    if (unread) {
        marked = create_tray_unread_icon(original.get(), ::GetSystemMetrics(SM_CXSMICON));
        if (!marked) return false;
    }
    auto next = notification;
    next.hIcon = unread ? marked.get() : original.get();
    if (!::Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &next)) return false;
    notification = next;
    original_ = std::move(original);
    marked_ = std::move(marked);
    unread_ = unread;
    return true;
}

} // namespace acecode::desktop
#endif
