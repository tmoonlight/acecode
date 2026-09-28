#pragma once
#include "tui/screen_port.hpp"
#include "tui/render_mode_factory.hpp"
#include "tui/redraw_pacer.hpp"
#include "tui/app/ui_post_target.hpp"
#include <atomic>
#include <memory>

namespace acecode::tui {
class TuiScreenHost final : public IScreenPort {
public:
    TuiScreenHost(ScreenRenderMode mode, const TuiConfig& config);
    std::weak_ptr<UiPostTarget> post_target() const { return post_target_; }
    ftxui::ScreenInteractive& screen() { return screen_; }
    bool hover_supported() const { return hover_supported_; }
    std::shared_ptr<TuiRedrawPacer> redraw_pacer() const { return redraw_pacer_; }
    std::atomic<std::int64_t>& last_keyboard_input_at_ms() { return last_keyboard_input_at_ms_; }
    bool request_scheduled_redraw(int minimum_interval_ms);
    void post_event(ftxui::Event event) override;
    void post_task(std::function<void()> task) override;
    void exit() override;
    std::string get_selection() override;
    void shift_selection(int dx, int dy) override;
    int dimx() const override;
private:
    ftxui::ScreenInteractive screen_;
    bool hover_supported_ = false;
    // Shared with posted frame-completion callbacks that finish after Draw/Flush.
    std::shared_ptr<TuiRedrawPacer> redraw_pacer_;
    std::atomic<std::int64_t> last_keyboard_input_at_ms_{0};
    // Shared during individual posts; all subscribers retain only weak leases.
    std::shared_ptr<UiPostTarget> post_target_;
    LifetimeToken lifetime_;
};
}
