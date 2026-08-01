#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acecode::desktop {

struct AgentBrowserBounds {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool visible = false;
};

struct AgentBrowserState {
    std::string page_id;
    bool supported = false;
    bool ready = false;
    bool loading = false;
    bool visible = false;
    bool active = false;
    bool closed = false;
    bool can_go_back = false;
    bool can_go_forward = false;
    std::string url = "about:blank";
    std::string title;
    std::string error;
};

// Native browser pages hosted in a dedicated child HWND above the ACECode main
// WebView. Each page owns its own controller/history while all pages share one
// isolated WebView2 environment/Profile.
// It intentionally exposes no host objects or web-message bridge to pages.
// The daemon-side agent reaches this exact page through the authenticated
// Desktop proxy published by agent_browser_runtime; no external CDP port is
// exposed.
class AgentBrowserHost {
public:
    using StateHandler = std::function<void(const AgentBrowserState&)>;
    using DispatchHandler = std::function<void(std::function<void()>)>;

    AgentBrowserHost(void* parent_window,
                     std::int64_t desktop_pid,
                     std::string desktop_instance_id,
                     StateHandler state_handler = {},
                     DispatchHandler dispatch_handler = {});
    ~AgentBrowserHost();

    AgentBrowserHost(const AgentBrowserHost&) = delete;
    AgentBrowserHost& operator=(const AgentBrowserHost&) = delete;

    bool supported() const;
    AgentBrowserState state(const std::string& page_id = {}) const;
    std::vector<AgentBrowserState> states() const;
    std::string active_page_id() const;

    std::string create_page(std::string* error = nullptr);
    bool close_page(const std::string& page_id, std::string* error = nullptr);
    bool select_page(const std::string& page_id, std::string* error = nullptr);
    bool set_bounds(const std::string& page_id,
                    const AgentBrowserBounds& bounds,
                    std::string* error = nullptr);
    bool navigate(const std::string& page_id,
                  const std::string& input,
                  std::string* error = nullptr);
    bool go_back(const std::string& page_id, std::string* error = nullptr);
    bool go_forward(const std::string& page_id, std::string* error = nullptr);
    bool reload(const std::string& page_id, std::string* error = nullptr);
    bool focus(const std::string& page_id, std::string* error = nullptr);
    void hide(const std::string& page_id = {});

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace acecode::desktop
