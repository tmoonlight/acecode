#pragma once

// 安装 / 卸载窗口:无边框分层窗口,卡片、控件和图标动画全部自绘(画法见
// installer_canvas_win 与 installer_timeline)。安装或卸载在工作线程上跑,
// 界面每帧读取进度。只在 Windows 上编译。

#include "install_engine.hpp"
#include "installer_strings.hpp"

#include <optional>
#include <string>

namespace acecode::installer::ui {

struct WindowConfig {
    InstallerMode mode = InstallerMode::install;
    InstallerStrings strings;
    std::optional<Payload> payload;
    std::optional<ExistingInstall> existing;
    std::string install_dir;
    bool add_to_path = true;
    bool desktop_shortcut = false;
};

// 返回进程退出码:完成为 0,取消或失败为 1。
int run_installer_window(const WindowConfig& config);

}  // namespace acecode::installer::ui
