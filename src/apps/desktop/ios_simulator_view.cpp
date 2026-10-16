// Platforms without CoreSimulator: the web panel keeps the JPEG stream.
#include "ios_simulator_view.hpp"

namespace acecode::desktop {

struct IosSimulatorView::Impl {
    StateHandler on_state;
};

IosSimulatorView::IosSimulatorView(void* parent_window, StateHandler state_handler)
    : impl_(std::make_shared<Impl>()) {
    (void)parent_window;
    impl_->on_state = std::move(state_handler);
}

IosSimulatorView::~IosSimulatorView() = default;

bool IosSimulatorView::supported() const { return false; }

bool IosSimulatorView::set_layout(const IosSimulatorViewLayout& layout, std::string* error) {
    (void)layout;
    if (error) *error = "the native iOS Simulator view is only available on macOS";
    return false;
}

void IosSimulatorView::unmount() {}

nlohmann::json IosSimulatorView::state() const { return {{"supported", false}}; }

} // namespace acecode::desktop
