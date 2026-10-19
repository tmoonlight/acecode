// 生产 Desktop WebHost + 独立 WebView2 配置目录(由 renderer_recovery_smoke.py 提供)。
// 页面第一次加载时把渲染进程堆撑爆(驱动脚本把 V8 老生代上限压到 64 MB),
// 宿主收到 ProcessFailed 后应自动 Reload;第二次加载确认桥接调用正常往返、
// 回包清理脚本已接管。用户反馈的场景:挂机一两天后渲染进程内存耗尽,窗口停在
// WebView2 的「内存不足」错误页,只能手动点刷新。
#include "desktop/web_host.hpp"
#include "utils/scope_exit.hpp"

#include <windows.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    using acecode::desktop::WebHost;
    WebHost host(false, WebHost::StartupWindowMode::OffscreenUntilReady);
    // Shared by this runner and the host-owned reporting callback.
    auto result = std::make_shared<int>(1);
    host.bind("aceRecoveryEcho", [](const std::string& message) {
        return nlohmann::json::parse(message).at(0).dump();
    });
    host.bind("aceRecoveryDone", [result](const std::string& message) {
        const auto report = nlohmann::json::parse(message).at(0);
        std::cout << report.dump() << std::endl;
        *result = report.value("ok", false) ? 0 : 1;
        ::PostQuitMessage(0);
        return std::string("null");
    });
    const auto timer = ::SetTimer(nullptr, 0, 60000,
        +[](HWND, UINT, UINT_PTR, DWORD) { ::PostQuitMessage(1); });
    if (!timer) return 3;
    acecode::ScopeExit stop_timer([timer] { ::KillTimer(nullptr, timer); });
    host.init_script(R"JS(
        addEventListener('DOMContentLoaded', async () => {
            const loads = Number(localStorage.getItem('ace-recovery-loads') || '0') + 1;
            localStorage.setItem('ace-recovery-loads', String(loads));
            if (loads === 1) {
                // 先让计数写出去,再撑爆堆。
                setTimeout(() => {
                    const hog = [];
                    for (;;) hog.push(new Array(1 << 20).fill(loads));
                }, 300);
                return;
            }
            try {
                const bridge = Object.getPrototypeOf(window.__webview__);
                const patched = bridge.__acecodeReplyCleanup === true;
                const echoes = [];
                for (let i = 0; i < 3; i++) echoes.push(await aceRecoveryEcho(i));
                aceRecoveryDone({ ok: patched && echoes.join(',') === '0,1,2', loads, patched, echoes });
            } catch (error) {
                aceRecoveryDone({ ok: false, loads, error: String(error) });
            }
        }, { once: true });
    )JS");
    host.navigate(argv[1]);
    host.run();
    return *result;
}
