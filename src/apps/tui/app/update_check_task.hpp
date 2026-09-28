#pragma once
#include "config/config.hpp"
#include "tui/tui_state.hpp"
#include "tui/screen_port.hpp"
#include "utils/joining_thread.hpp"
namespace acecode::tui {
class UpdateCheckTask {
public:
    UpdateCheckTask(const AppConfig& config, TuiState& state, IScreenPort& screen);
    void join() { worker_.join(); }
private:
    void run();
    const AppConfig config;
    TuiState& state;
    IScreenPort& screen;
    JoiningThread worker_;  // Joins before dependencies; this capture stays owned.
};
}
