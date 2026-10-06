#pragma once

// 桌面 agent 办公室:acecode-desktop 启动时在主屏右下角拉起一个透明、置顶、
// 不抢焦点、不进任务栏的小窗口,里面是真实会话驱动的像素 AI 办公室
// (assets/desktop_pet/agent_office_pet.html,构建期嵌进 exe)。
// Web 应用经快照桥更新；GUI 线程拥有窗口和桥回调。
//
// 交互:左键拖动挪位置;单击小人头顶报状态;右键菜单「回到右下角 / 隐藏桌面宠物」
// (隐藏只到本次退出)。环境变量 ACECODE_DESKTOP_PET=0 可整体关闭。
//
// Windows 实现在 desktop_pet.cpp(独立的 WebView2 环境,用户数据在
// %LOCALAPPDATA%\ACECode\desktop-pet\webview2);macOS 实现在 desktop_pet_mac.mm
// (非激活 NSPanel + WKWebView,设置在 ~/Library/Application Support/ACECode/desktop-pet)。
// 两者都不进 ACECode 数据目录,以免被数据目录迁移当成持续写入的活文件;
// Linux 构造即空操作。
// 必须在 GUI 线程构造与析构;任何失败只记日志,不影响主窗口。

#include <memory>

namespace acecode::desktop {

class WebHost;

class DesktopPet {
public:
    explicit DesktopPet(WebHost& host);
    ~DesktopPet();

    DesktopPet(const DesktopPet&) = delete;
    DesktopPet& operator=(const DesktopPet&) = delete;

private:
    struct Impl;
    // DesktopPet owns the state; WebView callbacks briefly share it via weak locks.
    std::shared_ptr<Impl> impl_;
};

} // namespace acecode::desktop
