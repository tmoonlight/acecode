#include "desktop/agent_browser_host.hpp"
#include "tool/agent_browser/cdp_client.hpp"

#import <AppKit/AppKit.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <unistd.h>

@interface ACECodeFlippedAgentBrowserSmokeView : NSView
@end

@implementation ACECodeFlippedAgentBrowserSmokeView
- (BOOL)isFlipped {
    return YES;
}
@end

namespace {

bool pump_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop]
                runMode:NSDefaultRunLoopMode
              beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
        if (predicate()) return true;
    }
    return predicate();
}

int fail(const std::string& message) {
    std::cerr << "SMOKE_FAILED: " << message << '\n';
    return 1;
}

class TempDir {
public:
    TempDir() {
        char path_template[] = "/tmp/acecode-ab-XXXXXX";
        if (char* created = ::mkdtemp(path_template)) {
            path_ = created;
        }
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

int main() {
    @autoreleasepool {
        TempDir isolated;
        if (isolated.path().empty()) {
            return fail("failed to create isolated runtime directory");
        }

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 900, 640)
                      styleMask:(NSWindowStyleMaskTitled |
                                 NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskResizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        ACECodeFlippedAgentBrowserSmokeView* shell_view =
            [[ACECodeFlippedAgentBrowserSmokeView alloc]
                initWithFrame:[[window contentView] bounds]];
        [shell_view setAutoresizingMask:
            NSViewWidthSizable | NSViewHeightSizable];
        [window setContentView:shell_view];
        [shell_view release];
        [window orderFront:nil];

        const std::string data_dir = isolated.path().string();
        int exit_code = 0;
        {
            acecode::desktop::AgentBrowserHost host(
                window,
                static_cast<std::int64_t>(::getpid()),
                "agent-browser-mac-smoke-" + std::to_string(::getpid()),
                {},
                [](std::function<void()> task) {
                    auto shared = std::make_shared<std::function<void()>>(
                        std::move(task));
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (*shared) (*shared)();
                    });
                },
                data_dir);

            if (!pump_until(
                    [&host] {
                        const auto state = host.state();
                        return state.ready || !state.error.empty();
                    },
                    std::chrono::seconds(10))) {
                exit_code = fail("WKWebView host did not become ready");
            } else if (!host.state().ready) {
                exit_code = fail(host.state().error);
            }

            std::string error;
            std::string page_id;
            std::string secondary_page_id;
            if (exit_code == 0) {
                page_id = host.create_page(&error);
                if (page_id.empty() ||
                    !host.set_bounds(page_id, {20, 20, 860, 580, true}, &error) ||
                    !host.set_shared_with_agent(page_id, true, &error) ||
                    !host.navigate(page_id, "about:blank", &error)) {
                    exit_code = fail(error.empty()
                        ? "failed to prepare WKWebView page" : error);
                }
            }
            if (exit_code == 0) {
                NSArray<NSView*>* native_pages =
                    [[window contentView] subviews];
                if ([native_pages count] != 1 ||
                    NSMinY([[native_pages firstObject] frame]) != 20) {
                    exit_code = fail(
                        "WKWebView frame did not preserve the flipped shell origin");
                }
            }
            if (exit_code == 0 &&
                !pump_until(
                    [&host, &page_id] {
                        return host.state(page_id).content_state ==
                            acecode::desktop::kAgentBrowserContentStateLive;
                    },
                    std::chrono::seconds(15))) {
                exit_code = fail("about:blank did not finish loading");
            }
            if (exit_code == 0) {
                secondary_page_id = host.create_page(&error);
                if (secondary_page_id.empty() ||
                    secondary_page_id == page_id ||
                    !host.set_bounds(
                        secondary_page_id, {20, 20, 860, 580, true}, &error) ||
                    !host.set_shared_with_agent(
                        secondary_page_id, true, &error) ||
                    !host.navigate(secondary_page_id, "about:blank", &error)) {
                    exit_code = fail(error.empty()
                        ? "failed to prepare second WKWebView page" : error);
                }
            }
            if (exit_code == 0 &&
                !pump_until(
                    [&host, &secondary_page_id] {
                        return host.state(secondary_page_id).content_state ==
                            acecode::desktop::kAgentBrowserContentStateLive;
                    },
                    std::chrono::seconds(15))) {
                exit_code = fail("second about:blank page did not finish loading");
            }
            if (exit_code == 0 && host.states().size() != 2) {
                exit_code = fail("macOS host did not retain two independent pages");
            }

            std::atomic<bool> worker_done{false};
            std::string worker_error;
            bool synthetic_ok = false;
            bool screenshot_ok = false;
            std::thread worker;
            if (exit_code == 0) {
                worker = std::thread([&] {
                    acecode::agent_browser::AgentBrowserCdpClient client(data_dir);
                    if (!client.connect(
                            std::chrono::seconds(10), nullptr, worker_error) ||
                        !client.select_page(
                            page_id, std::chrono::seconds(10), nullptr,
                            worker_error)) {
                        worker_done.store(true);
                        return;
                    }
                    client.command(
                        "Runtime.evaluate",
                        {{"expression",
                          "(() => { document.body.innerHTML='<input id=field>'; document.querySelector('#field').focus(); return true; })()"},
                         {"awaitPromise", true}, {"returnByValue", true}},
                        std::chrono::seconds(10), nullptr, worker_error);
                    if (worker_error.empty()) {
                        client.command(
                            "Input.insertText",
                            {{"text", "synthetic-ok"},
                             {"acecodeInputMode", "synthetic"}},
                            std::chrono::seconds(10), nullptr, worker_error);
                    }
                    if (worker_error.empty()) {
                        const auto evaluated = client.command(
                            "Runtime.evaluate",
                            {{"expression",
                              "document.querySelector('#field').value"},
                             {"awaitPromise", true}, {"returnByValue", true}},
                            std::chrono::seconds(10), nullptr, worker_error);
                        synthetic_ok = evaluated.value("result", nlohmann::json::object())
                            .value("value", "") == "synthetic-ok";
                    }
                    if (worker_error.empty()) {
                        const auto captured = client.command(
                            "Page.captureScreenshot",
                            {{"format", "png"}},
                            std::chrono::seconds(20), nullptr, worker_error);
                        const auto bytes =
                            acecode::agent_browser::decode_agent_browser_base64(
                                captured.value("data", ""));
                        screenshot_ok = bytes.has_value() &&
                            acecode::agent_browser::agent_browser_png_dimensions(
                                *bytes).has_value();
                    }
                    worker_done.store(true);
                });
                if (!pump_until(
                        [&worker_done] { return worker_done.load(); },
                        std::chrono::seconds(35))) {
                    exit_code = fail("macOS proxy smoke timed out");
                }
                worker.join();
                if (exit_code == 0 && !worker_error.empty()) {
                    exit_code = fail(worker_error);
                } else if (exit_code == 0 && !synthetic_ok) {
                    exit_code = fail("synthetic input did not update the field");
                } else if (exit_code == 0 && !screenshot_ok) {
                    exit_code = fail("WKWebView screenshot was not a valid PNG");
                }
            }

            if (!secondary_page_id.empty()) {
                std::string close_error;
                if (!host.close_page(secondary_page_id, &close_error) &&
                    exit_code == 0) {
                    exit_code = fail(close_error.empty()
                        ? "failed to close second WKWebView page" : close_error);
                } else if (exit_code == 0) {
                    const auto remaining = host.states();
                    if (remaining.size() != 1 ||
                        remaining.front().page_id != page_id ||
                        !remaining.front().active ||
                        remaining.front().content_state !=
                            acecode::desktop::kAgentBrowserContentStateLive) {
                        exit_code = fail(
                            "closing the second page changed the first page lifecycle");
                    }
                }
            }
            if (!page_id.empty()) {
                std::string ignored;
                host.close_page(page_id, &ignored);
            }
            if (exit_code == 0) {
                std::cout << "SMOKE_OK page_id=" << page_id
                          << " pages=2 synthetic=true screenshot=true\n";
            }
        }
        [window close];
        [window release];
        return exit_code;
    }
}
