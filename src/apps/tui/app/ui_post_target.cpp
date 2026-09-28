#include "tui/app/ui_post_target.hpp"
#include "tui/app/tui_screen_host.hpp"
namespace acecode::tui {
void UiPostTarget::post_task(std::function<void()> task) const {
    host_.with([&](TuiScreenHost& host) { host.post_task(std::move(task)); });
}
void UiPostTarget::post_event(ftxui::Event event) const {
    host_.with([&](TuiScreenHost& host) { host.post_event(std::move(event)); });
}
}
