#pragma once

#include "channels/setup.hpp"
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <memory>

namespace acecode::tui {

struct ChannelsSetupDependencies {
    std::function<void()> request_close;
    std::function<void()> post_event;
    channels::SetupDependencies setup;
    bool english = false;
    // Only the standalone command supplies this to request content-sized output.
    std::function<ftxui::Dimensions()> terminal_dimensions;
};

class ChannelsSetup {
public:
    explicit ChannelsSetup(ChannelsSetupDependencies dependencies);
    ~ChannelsSetup();
    ftxui::Component component() const;
    void open();
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

ftxui::ScreenInteractive make_channels_setup_terminal();
int run_channels_setup();
} // namespace acecode::tui
