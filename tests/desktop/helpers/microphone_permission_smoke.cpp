// Uses the production Desktop WebHost and a fresh WebView2 profile. The driver
// supplies fake audio hardware, but deliberately does not auto-accept prompts.
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
    host.bind("aceMicrophoneTestDone", [result](const std::string& message) {
        const auto report = nlohmann::json::parse(message).at(0);
        std::cout << report.dump() << std::endl;
        *result = report.value("ok", false) ? 0 : 1;
        ::PostQuitMessage(0);
        return std::string("null");
    });
    const auto timer = ::SetTimer(nullptr, 0, 15000,
        +[](HWND, UINT, UINT_PTR, DWORD) { ::PostQuitMessage(1); });
    if (!timer) return 3;
    acecode::ScopeExit stop_timer([timer] { ::KillTimer(nullptr, timer); });
    host.init_script(R"JS(
        addEventListener('DOMContentLoaded', async () => {
            const states = [];
            try {
                for (let attempt = 0; attempt < 2; attempt++) {
                    const before = (await navigator.permissions.query({ name: 'microphone' })).state;
                    const stream = await navigator.mediaDevices.getUserMedia({ audio: true, video: false });
                    const live = stream.getAudioTracks().some(track => track.readyState === 'live');
                    stream.getTracks().forEach(track => track.stop());
                    const after = (await navigator.permissions.query({ name: 'microphone' })).state;
                    states.push({ before, after, live });
                }
                // Chromium can retain a temporary grant within the current page.
                // A fresh host using the same profile must still start at prompt.
                aceMicrophoneTestDone({ ok: states[0].before === 'prompt' && states.every(s => s.live), states });
            } catch (error) {
                aceMicrophoneTestDone({ ok: false, error: error.name, states });
            }
        }, { once: true });
    )JS");
    host.navigate(argv[1]);
    host.run();
    return *result;
}
