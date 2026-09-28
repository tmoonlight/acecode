#include "tui/app/update_check_task.hpp"
#include "upgrade/check.hpp"
#include "utils/logger.hpp"
#include "version.hpp"
using ftxui::Event;
namespace acecode::tui {
UpdateCheckTask::UpdateCheckTask(const AppConfig& cfg, TuiState& s, IScreenPort& scr)
    : config(cfg), state(s), screen(scr), worker_([this] { run(); }) {}
void UpdateCheckTask::run() {
    try {
        auto result = acecode::upgrade::check_for_update(config, ACECODE_VERSION);
        if (result.update_available()) {
            std::lock_guard<std::mutex> lk(state.mu);
            state.update_notice = "Update available: v" + result.latest_version +
                                  ". Run acecode update.";
            screen.post_event(Event::Custom);
        } else if (result.status != acecode::upgrade::UpdateCheckStatus::UpToDate) {
            LOG_DEBUG(std::string("[upgrade] startup check skipped: ") +
                      acecode::upgrade::update_check_status_name(result.status) +
                      (result.error.empty() ? "" : " (" + result.error + ")"));
        }
    } catch (const std::exception& e) {
        LOG_DEBUG(std::string("[upgrade] startup check failed: ") + e.what());
    } catch (...) {
        LOG_DEBUG("[upgrade] startup check failed with unknown exception");
    }
}
}
