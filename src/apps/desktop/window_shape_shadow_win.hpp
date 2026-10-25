#pragma once

#ifdef _WIN32

#include <windows.h>

#include <array>

#include "window_shape.hpp"

namespace acecode::desktop {

// 切角窗口的阴影。窗口设置区域(SetWindowRgn)后系统阴影整圈消失,这里用四块
// 分层弹出窗口补画:归主窗口所有(随主窗口最小化 / 还原,不进任务栏与 Alt+Tab),
// WS_EX_TRANSPARENT 让鼠标穿透,按 chamfer_shadow_strips 贴在主窗口四周。
// 上下两块向内覆盖切角高度,形内像素全透明,切角缺口处正好露出阴影。
// 同一处还管四块缩放把手窗口(chamfer_resize_zone):alpha 1 的像素接住鼠标,
// 命中返回 HTTOPLEFT 等让系统给出缩放光标,按下时把缩放交给主窗口。
// GUI 主线程 only。
class ChamferShadowWindows {
public:
    ChamferShadowWindows() = default;
    ChamferShadowWindows(const ChamferShadowWindows&) = delete;
    ChamferShadowWindows& operator=(const ChamferShadowWindows&) = delete;
    ~ChamferShadowWindows();

    // 按 owner 当前窗口矩形摆放;尺寸、切角、样式或把手尺寸变了才重画,只是移动时仅挪位置。
    // grip 为窗口外侧把手的厚度,reach 为角上把手沿边延伸的长度(像素)。
    void update(HWND owner, int cut, const ShadowStyle& style, int grip, int reach);
    void hide();
    void destroy();

private:
    bool ensure_created(HWND owner);
    static LRESULT CALLBACK grip_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT grip_hit_test(LPARAM lparam) const;

    std::array<HWND, 4> windows_{};
    std::array<HWND, 4> grips_{};
    HWND owner_ = nullptr;
    int last_width_ = -1;
    int last_height_ = -1;
    int last_cut_ = -1;
    int last_grip_ = -1;
    int last_reach_ = -1;
    ShadowStyle last_style_{};
    bool rendered_ = false;
    bool visible_ = false;
};

} // namespace acecode::desktop

#endif // _WIN32
