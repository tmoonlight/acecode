#include "tui/app/tui_screen_host.hpp"
#include "platform/terminal/terminal_capability.hpp"
#include <utility>

namespace acecode::tui {
TuiScreenHost::TuiScreenHost(ScreenRenderMode mode, const TuiConfig& config)
    : screen_(make_screen_interactive(mode)) {
    screen_.EnableKittyKeyboard();
    hover_supported_ = acecode::detect_hover_motion_support();
    screen_.EnableMouseHoverMotion(hover_supported_);
    screen_.EnableSynchronizedOutput(decide_synchronized_output(
        config, acecode::detect_synchronized_output_support()));
    redraw_pacer_ = std::make_shared<TuiRedrawPacer>();
    post_target_ = std::make_shared<UiPostTarget>(lifetime_.ref(*this));
}
bool TuiScreenHost::request_scheduled_redraw(int minimum_interval_ms) {
    if (!redraw_pacer_->try_request_scheduled_redraw(
            monotonic_milliseconds(), minimum_interval_ms)) return false;
    post_event(ftxui::Event::Custom);
    return true;
}
void TuiScreenHost::post_event(ftxui::Event event) { screen_.PostEvent(std::move(event)); }
void TuiScreenHost::post_task(std::function<void()> task) { screen_.Post(std::move(task)); }
void TuiScreenHost::exit() { screen_.Exit(); }
std::string TuiScreenHost::get_selection() { return screen_.GetSelection(); }
void TuiScreenHost::shift_selection(int dx, int dy) { screen_.ShiftSelection(dx, dy); }
int TuiScreenHost::dimx() const { return screen_.dimx(); }

void TuiScreenHost::activate() {
    if (!active_screen_) active_screen_.emplace(screen_);
}
void TuiScreenHost::deactivate() {
    if (active_screen_) active_screen_->release();
}

}
