#include "tui/app/tui_app.hpp"
#include "tui/app/tui_services.hpp"
#include "tui/app/tui_screen_host.hpp"
#include "tui/app/model_pool_monitor_subscription.hpp"
#include "tui/app/tui_notification_binding.hpp"
#include "tui/app/inbound_submit_registration.hpp"
#include "tui/app/mcp_status_binding.hpp"
#include "tui/app/animation_ticker.hpp"
#include "tui/app/copilot_auth_task.hpp"
#include "tui/app/update_check_task.hpp"
#include "tui/app/process_guards.hpp"
#include "tui/app/startup_worktree.hpp"
#include "agent/agent_loop.hpp"
#include "tui/subagent_host.hpp"
#include "session_host/auto_title_runner.hpp"
#include "tool/mcp_manager.hpp"
#include "lsp/lsp_service.hpp"
#include "tool/web_search/runtime.hpp"
#include "utils/abandonable_call.hpp"
#include "platform/power_inhibitor.hpp"
#include "remote_control/remote_control_service.hpp"
#include <iostream>
namespace acecode::tui {
void TuiApp::shutdown_step(TuiShutdownStep step) {
    switch (step) {
    case TuiShutdownStep::ModelPool:
        if (model_pool_monitor_) model_pool_monitor_->stop();
        break;
    case TuiShutdownStep::AutoTitle:
        if (auto_title_runner_) auto_title_runner_->stop();
        break;
    case TuiShutdownStep::Notifications:
        if (notifications_) notifications_->shutdown();
        break;
    case TuiShutdownStep::ActiveScreen:
        if (screen_host_) screen_host_->deactivate();
        break;
    case TuiShutdownStep::ConsoleHandler:
        if (console_ctrl_) console_ctrl_->release();
        break;
    case TuiShutdownStep::StopAnimation:
        if (animation_) animation_->request_stop();
        break;
    case TuiShutdownStep::InboundSubmit:
        if (inbound_submit_) inbound_submit_->stop();
        else if (services_) rc::remote_control_service().stop();
        break;
    case TuiShutdownStep::AbortAndWake: {
        agent_aborting_ = true;
        if (agent_loop_) agent_loop_->abort();
        std::lock_guard<std::mutex> lock(state_.mu);
        if (state_.confirm_pending) {
            state_.confirm_pending = false;
            state_.confirm_result = PermissionResult::Deny;
            state_.confirm_cv.notify_one();
        }
        if (state_.ask_pending) {
            state_.ask_pending = false;
            state_.ask_completion_override.reset();
            state_.ask_cv.notify_one();
        }
        state_.remote_confirm_queue.clear();
        state_.overlay_cv.notify_all();
        break;
    }
    case TuiShutdownStep::AgentWorker:
        if (agent_loop_) agent_loop_->shutdown();
        break;
    case TuiShutdownStep::Subagents:
        if (subagent_host_) subagent_host_->shutdown();
        break;
    case TuiShutdownStep::PowerLease:
        if (agent_loop_) release_process_session_power(kTuiMainPowerSessionId);
        break;
    case TuiShutdownStep::Mcp:
        if (services_ && services_->mcp) services_->mcp->shutdown();
        if (mcp_status_) mcp_status_->stop();
        break;
    case TuiShutdownStep::Lsp:
        if (services_) lsp::shutdown();
        break;
    case TuiShutdownStep::CompactWorker:
        state_.compact_abort_requested.store(true);
        if (state_.compact_thread.joinable()) state_.compact_thread.join();
        break;
    case TuiShutdownStep::AnimationWorker:
        if (animation_) animation_->join();
        break;
    case TuiShutdownStep::AuthWorker:
        if (auth_task_) auth_task_->join();
        break;
    case TuiShutdownStep::UpdateWorker:
        if (update_check_) update_check_->join();
        break;
    case TuiShutdownStep::Worktree:
        if (main_session_started_) finalize_session_worktree_on_exit(session_manager_);
        break;
    case TuiShutdownStep::FinalizeSession:
        if (main_session_started_) session_manager_.finalize();
        break;
    case TuiShutdownStep::CleanupSessions:
        if (main_session_started_) session_manager_.cleanup_old_sessions(services_->config.max_sessions);
        break;
    case TuiShutdownStep::SessionRegistration:
        if (main_session_started_) exit_session_id_ = session_manager_.current_session_id();
        if (session_finalize_) session_finalize_->release();
        break;
    case TuiShutdownStep::ResumeHint:
        if (!exit_session_id_.empty())
            std::cerr << "\nacecode: session " << exit_session_id_
                      << " saved. Resume with: acecode --resume " << exit_session_id_ << std::endl;
        break;
    case TuiShutdownStep::AbandonedWork:
        web_search::shutdown();
        wait_for_abandoned_work(std::chrono::seconds(2));
        break;
    }
}
}
