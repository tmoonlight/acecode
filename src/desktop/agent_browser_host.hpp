#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acecode::desktop {

inline constexpr const char kAgentBrowserDefaultTitle[] = u8"新标签页";
inline constexpr const char kAgentBrowserContentStateEmpty[] = "empty";
inline constexpr const char kAgentBrowserContentStateLoading[] = "loading";
inline constexpr const char kAgentBrowserContentStateLive[] = "live";
inline constexpr const char kAgentBrowserContentStateNavigationError[] =
    "navigation_error";
inline constexpr const char kAgentBrowserContentStateProcessFailed[] =
    "process_failed";

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
    bool shared_with_agent = false;
    bool element_selection_active = false;
    std::uint64_t element_selection_serial = 0;
    std::string url = "about:blank";
    std::string title = kAgentBrowserDefaultTitle;
    std::string favicon;
    std::string content_state = kAgentBrowserContentStateEmpty;
    std::string failure_kind;
    std::string error;
    // Transient event payload. It is populated only on the state snapshot that
    // completes a user element pick and is not retained in the page state.
    std::string selected_element_json;
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
    bool set_shared_with_agent(const std::string& page_id,
                               bool shared,
                               std::string* error = nullptr);
    bool toggle_element_selection(const std::string& page_id,
                                  std::string* error = nullptr);
    std::string console_logs(const std::string& page_id,
                             std::string* error = nullptr) const;
    bool open_developer_tools(const std::string& page_id,
                              std::string* error = nullptr);
    void hide(const std::string& page_id = {});

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace acecode::desktop
