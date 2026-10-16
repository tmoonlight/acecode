#pragma once

// Native iOS Simulator screen for the Desktop shell. On macOS the booted
// simulator's IOSurface is set as the contents of a layer placed over the
// web panel's screen area, so frames reach the window server without being
// copied, scaled or JPEG-encoded. The view never takes input: hit testing
// passes through to the web panel, which keeps sending touches and keys to
// the daemon. Other platforms report unsupported and the web panel keeps the
// JPEG stream.
//
// Only CoreSimulator is loaded (fixed system path); the developer directory
// from the web page is passed to CoreSimulator as a string and never used to
// load code.

#include "ios_simulator_view_layout.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>

namespace acecode::desktop {

class WebHost;

class IosSimulatorView {
public:
    // Receives {udid, attached, rendering, rotation, pixel_width,
    // pixel_height, layout_revision, error} on the UI thread.
    using StateHandler = std::function<void(const nlohmann::json&)>;

    IosSimulatorView(void* parent_window, StateHandler state_handler);
    ~IosSimulatorView();
    IosSimulatorView(const IosSimulatorView&) = delete;
    IosSimulatorView& operator=(const IosSimulatorView&) = delete;

    bool supported() const;
    // Attaches to layout.udid (asynchronously the first time) and places the
    // screen. Call on the UI thread.
    bool set_layout(const IosSimulatorViewLayout& layout, std::string* error);
    void unmount();
    nlohmann::json state() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Creates the view over the host window and binds
// aceDesktop_iosSimulatorViewSetLayout([layout]) / ...Unmount() /
// ...GetState(), each answering {ok, ...state} or {ok:false, error}. State
// changes reach the page as `acecode:ios-simulator-view-state` events. The
// view is a subview of the window's content view, so it hides with the window;
// the page re-sends the layout whenever the panel moves or resizes.
std::unique_ptr<IosSimulatorView> install_ios_simulator_view(WebHost& host);

} // namespace acecode::desktop
