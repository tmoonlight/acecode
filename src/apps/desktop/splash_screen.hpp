#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace acecode::desktop {

class SplashScreen {
public:
    SplashScreen();
    ~SplashScreen();

    SplashScreen(const SplashScreen&) = delete;
    SplashScreen& operator=(const SplashScreen&) = delete;

    void show();
    void set_status(const std::string& message, std::uint64_t elapsed_ms);
    // 首屏就绪:播完图标收尾('A' 擦入、'>' 砸上),到该显示主窗口时调用 on_reveal,随后淡出自行关闭。
    // Windows 上 on_reveal 在启动浮窗的绘制线程上触发;其它平台立即关闭并在调用线程上触发。
    // close() 之后不会再调用 on_reveal。
    void finish(std::function<void()> on_reveal);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::desktop
