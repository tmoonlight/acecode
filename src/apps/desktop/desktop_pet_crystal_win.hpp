#pragma once

// 水晶球桌宠的「透镜」(仅 Windows):页面 assets/desktop_pet/crystal_pet.js 用 WebGL 把球背后的桌面
// 按实心玻璃球的光路折射出来,这里负责提供「球背后的桌面」。
//   1. 宠物窗口设 WDA_EXCLUDEFROMCAPTURE,抓屏时它不存在,抓到的就是背后的内容。Win11 上逐像素透明的
//      窗口不带 CAPTUREBLT 也会被 BitBlt 抓到,不排除的话球会把自己拍进去层层套娃。
//      代价:截图 / 录屏里也看不到这个宠物窗口。
//   2. 抓屏线程按页面上报的球位置(crystal-lens)抓球周围一圈,写进 WebView2 共享内存,GUI 线程再发
//      crystal-frame 通知页面;页面上传纹理后回 crystal-ack,收到确认才抓下一帧,所以写共享内存时
//      页面不会正在读。画面和位置都没变(哈希相同)就不发。
//   3. 远程桌面会话里显示亲和性可能让窗口在远端整个看不见:远程时不排除、不抓屏,
//      发 crystal-off 让页面退回内置背景,会话切回本机再恢复。
//   4. 鼠标位置(crystal-cursor)也从这里发:鼠标在窗口外时页面收不到事件,眼睛靠它跟着鼠标看。
// GUI 线程构造、调用、析构;析构先停抓屏线程,再关共享内存、撤销亲和性。

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

#  include <memory>

#  include <nlohmann/json_fwd.hpp>

struct ICoreWebView2;
struct ICoreWebView2Environment;

namespace acecode::desktop {

class CrystalLens {
public:
    // captured_message:抓屏线程抓完一帧后投递给 window 的消息,窗口过程收到后调用 on_captured()。
    CrystalLens(HWND window, UINT captured_message, ICoreWebView2Environment* environment, ICoreWebView2* webview);
    ~CrystalLens();

    CrystalLens(const CrystalLens&) = delete;
    CrystalLens& operator=(const CrystalLens&) = delete;

    // 页面发来的 crystal-lens / crystal-ack。
    void handle(const nlohmann::json& message);
    // GUI 计时器(约 60Hz):发鼠标位置,空闲且页面已确认上一帧时安排下一次抓屏。
    void tick();
    // 抓屏线程完成一帧(窗口过程收到 captured_message)。
    void on_captured();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::desktop
#endif
