#pragma once

// 桌面宠物(像素办公室 demo)窗口的纯几何计算,不依赖任何平台 API,
// 由 desktop_pet.cpp 的 Win32 宿主调用,单测在 tests/desktop/desktop_pet_layout_test.cpp。
//
// 场景是 assets/desktop_pet/agent_office_pet.html 里 344×220 的像素画布。
// 缩放倍数 scale = 每个场景像素占多少设备像素:首次运行取整数倍(像素最整齐),
// 用户用滚轮 / 把手缩放后可以是任意小数,页面负责把非整数倍画得均匀。
// 画布里房间以外(原来的草地)是透明的,命中区域只取房间轮廓,
// 透明角落的点击会落到后面的窗口上。

#include <array>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace acecode::desktop {

inline constexpr int kDesktopPetSceneWidth = 344;
inline constexpr int kDesktopPetSceneHeight = 252;
// Scene layout reserves this space even while the controls are hidden.
inline constexpr int kDesktopPetToolbarHeight = 32;

struct DesktopPetRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct DesktopPetPoint {
    int x = 0;
    int y = 0;
};

struct DesktopPetPlacement {
    DesktopPetRect window;
    double scale = 1.0;
};

// 首次运行的默认倍数(整数)。目标是逻辑宽度约 430px(dpi/96 × 1.25 四舍五入),
// 至少 1;放不下时逐级缩小,保证窗口不超过工作区宽/高的一半。
int desktop_pet_scale(int dpi, int work_width, int work_height);

// 用户缩放的上下限:最小是逻辑 0.5 倍(172 逻辑像素宽),最大不超过工作区宽/高的 90%。
// 非法输入(NaN、负数)按下限处理。
double clamp_desktop_pet_scale(double scale, int dpi, const DesktopPetRect& work_area);

// 把窗口贴在工作区(已扣除任务栏)右下角,留 8 个逻辑像素边距。
// preferred_logical_scale > 0 表示用户调过大小(逻辑倍数,与 DPI 无关),否则用默认整数倍。
DesktopPetPlacement place_desktop_pet(const DesktopPetRect& work_area, int dpi,
                                      double preferred_logical_scale = 0.0);

// 保持左上角不动、只按新 DPI 换尺寸;用户拖动过之后 DPI 变化走这条。
DesktopPetPlacement resize_desktop_pet(const DesktopPetRect& current, int dpi,
                                       const DesktopPetRect& work_area,
                                       double preferred_logical_scale = 0.0);

// 缩放到 new_scale(调用方先 clamp):窗口内相对位置 (anchor_fx, anchor_fy) 那一点
// 在屏幕上保持不动,再把整个窗口夹回工作区。停靠右下角时锚点取 (1, 1)。
DesktopPetPlacement scale_desktop_pet(const DesktopPetRect& current, double new_scale,
                                      double anchor_fx, double anchor_fy,
                                      const DesktopPetRect& work_area);

// 房间轮廓(地台 + 两面墙的外包六边形)外扩少量边距,按 scale 换成窗口内设备像素坐标,
// 顺时针,已夹在窗口范围内。
std::vector<DesktopPetPoint> desktop_pet_hit_polygon(double scale);

// 页面浮层(可见控制条、成员列表、提示、状态气泡)在窗口里的矩形,按窗口宽高的比例给出:
// {left, top, right, bottom},均在 [0, 1]。
using DesktopPetOverlay = std::array<double, 4>;
inline constexpr std::size_t kDesktopPetMaxOverlays = 24;

// 解析页面发来的 {"type":"overlay","controls":[...]|null,"rect":[...]|null,"bubbles":[[...],...]}:
// 非数字 / 非有限值 / 空矩形整条丢弃,坐标夹到 [0, 1],最多保留 kDesktopPetMaxOverlays 个。
std::vector<DesktopPetOverlay> desktop_pet_overlays_from_message(const nlohmann::json& message);

// 窗口内一点 (x, y)(设备像素,左上为原点)是否落在桌宠上:房间轮廓或页面上报的可见浮层。
// 透明区域返回 false,点击应穿透到后面的窗口。
bool desktop_pet_hit_test(double scale, int width, int height,
                          const std::vector<DesktopPetOverlay>& overlays, double x, double y);

// macOS uses logical points (CSS pixels). The handle's position scales with the
// viewport, but its 16px target does not; keep this aligned with .grip in the page.
bool desktop_pet_resize_grip_hit_test(double width, double height, double x, double y);

} // namespace acecode::desktop
