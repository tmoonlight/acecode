#pragma once

#include <functional>
#include <optional>
#include <string>

namespace acecode::desktop {

inline constexpr double kMinDesktopZoomFactor = 0.5;
inline constexpr double kMaxDesktopZoomFactor = 3.0;

// 非有限值回到 100%,其余收进 [50%, 300%]。
double clamp_desktop_zoom_factor(double factor);

// 主窗口页面缩放。前端拦下 Ctrl+滚轮后经 aceDesktop_zoomFactor 调用:
// [] 查询当前比例,[factor] 设置;进程内记住当前值,启动时从 state.json 恢复、
// 退出时写回。只在宿主 GUI 线程使用(bind 回调天然满足)。
class DesktopZoom {
public:
    using Apply = std::function<bool(double factor)>;

    explicit DesktopZoom(Apply apply);

    // 启动时套用上次的比例(没有就是 100%);套用失败说明当前平台不支持,之后的查询都报不可用。
    void restore(std::optional<double> saved);
    std::string handle_bridge(const std::string& request);

    double factor() const { return factor_; }
    // 与启动时恢复的值不同才需要写回 state.json。
    bool changed() const { return factor_ != restored_; }

private:
    Apply apply_;
    bool supported_ = false;
    double factor_ = 1.0;
    double restored_ = 1.0;
};

// state.json 的 desktop_zoom_factor:缺失 / 非数值 → nullopt,永不抛异常。
std::optional<double> read_desktop_zoom_factor();
void write_desktop_zoom_factor(double factor);

} // namespace acecode::desktop
