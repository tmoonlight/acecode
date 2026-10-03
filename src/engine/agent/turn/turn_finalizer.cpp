#include "turn_finalizer.hpp"
#include "turn_context.hpp"
#include "active_turn_gate.hpp"
#include "turn_outcome.hpp"
#include "agent/callbacks_slot.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "agent/transcript/trajectory_recorder.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "agent/progress/activity_narrator.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "session/session_manager.hpp"
#include "session/event_dispatcher.hpp"
#include "session/system_notice.hpp"
#include "session/thread_goal_store.hpp"
#include "session/turn_net_diff.hpp"
#include "tool/tool_executor.hpp"
#include "utils/abort_signal.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/time.hpp"
#include "utils/uuid.hpp"

namespace acecode::agent {
using detail::model_step_usage_to_json;
using detail::trailing_transcript_message;
using utils::now_epoch_ms;

enum class TurnFinalizer::Mode { Normal = 1, Hook = 2, Recovery = 4 };
enum class TurnFinalizer::Step {
    PrepareNormal, PrepareRecovery, StopGoalRecovery, ReportError, AccountHook,
    TurnFinished, BuildPayloads, Trajectory, BusyCallback, CloseNormal,
    RecordOutcome, ClearBusy, BusyEvent, Done, AfterNormal, ContinueHook
};
struct TurnFinalizer::Frame {
    Mode mode;
    TurnContext* turn;
    bool chat;
    int max_iterations;
    std::string error;
    LifetimeRef<TrajectoryRecorder> trajectory;
    std::string turn_id;
    nlohmann::json idle;
    nlohmann::json done;
};

TurnFinalizer::TurnFinalizer(TurnFinalizerServices services)
    : history_(services.history),
      transcript_(services.transcript),
      outcome_(services.outcome),
      gate_(services.gate),
      goal_(services.goal),
      hooks_(services.hooks),
      activity_(services.activity),
      events_(services.events),
      callbacks_(services.callbacks),
      tools_(services.tools),
      policy_(services.policy),
      busy_(services.busy),
      abort_signal_(services.abort_signal),
      turn_interrupt_requested_(services.turn_interrupt_requested),
      session_manager_(services.session_manager) {}

void TurnFinalizer::normal(TurnContext& turn, int max_iterations,
    LifetimeRef<TrajectoryRecorder> trajectory) {
    Frame frame{Mode::Normal, &turn, true, max_iterations, {}, trajectory, {}, {}, {}};
    run(frame);
}
void TurnFinalizer::hook_blocked(TurnContext& turn, const std::string& reason,
    LifetimeRef<TrajectoryRecorder> trajectory) {
    Frame frame{Mode::Hook, &turn, true, 0, "[Hook blocked prompt] " + reason,
        trajectory, {}, {}, {}};
    run(frame);
}
void TurnFinalizer::recover(TurnContext* turn, const char* detail, bool chat,
    LifetimeRef<TrajectoryRecorder> trajectory) {
    const std::string error = "[Error] Task failed: " + ensure_utf8(detail);
    LOG_ERROR(error);
    Frame frame{Mode::Recovery, turn, chat, 0, error, trajectory, {}, {}, {}};
    run(frame);
}

void TurnFinalizer::run(Frame& frame) {
    constexpr int normal = 1, hook = 2, recovery = 4, all = 7;
    struct Entry { Step step; int modes; bool isolate_recovery = false; };
    static constexpr Entry steps[] = {
        {Step::PrepareNormal, normal}, {Step::PrepareRecovery, recovery},
        {Step::StopGoalRecovery, recovery, true},
        {Step::ReportError, hook | recovery, true}, {Step::AccountHook, hook},
        {Step::TurnFinished, all, true}, {Step::BuildPayloads, all},
        {Step::Trajectory, all, true}, {Step::BusyCallback, all, true},
        {Step::CloseNormal, normal}, {Step::RecordOutcome, normal | hook},
        {Step::ClearBusy, normal | hook}, {Step::BusyEvent, all, true},
        {Step::Done, all, true}, {Step::AfterNormal, normal}, {Step::ContinueHook, hook},
    };
    for (const auto entry : steps) {
        if (!(entry.modes & static_cast<int>(frame.mode))) continue;
        if (frame.mode != Mode::Recovery || !entry.isolate_recovery) {
            step(entry.step, frame);
            continue;
        }
        try { step(entry.step, frame); }
        catch (const std::exception& error) {
            LOG_ERROR(std::string("Task error reporting failed: ") + error.what());
        } catch (...) {
            LOG_ERROR("Task error reporting failed with unknown exception");
        }
    }
}

void TurnFinalizer::step(Step selected, Frame& frame) {
    const auto callbacks = callbacks_.snapshot();
    const std::string status = frame.mode == Mode::Normal ? frame.turn->timing_status : "error";
    switch (selected) {
    case Step::PrepareNormal: prepare_normal(frame); break;
    case Step::PrepareRecovery:
        frame.turn_id = gate_.id();
        gate_.close_and_discard();
        turn_interrupt_requested_ = false;
        hooks_.clear_context();
        outcome_.set_error(frame.error);
        outcome_.record("error");
        busy_ = false;
        break;
    case Step::StopGoalRecovery:
        goal_.stop_after_error(session_manager_, ProviderErrorInfo{});
        break;
    case Step::ReportError: message("error", frame.error, false); break;
    case Step::AccountHook: goal_.account_usage(session_manager_, 0, false); break;
    case Step::TurnFinished:
        if (frame.chat && callbacks.on_turn_finished) callbacks.on_turn_finished(status);
        break;
    case Step::BuildPayloads: {
        if (frame.mode == Mode::Normal) frame.turn_id = frame.turn->info.active_turn_id;
        else if (frame.mode == Mode::Hook) frame.turn_id = generate_uuid();
        frame.idle = {{"busy", false}, {"outcome", status}, {"turn_id", frame.turn_id}};
        frame.done = {{"outcome", status}};
        if (frame.chat) {
            const auto usage = model_step_usage_to_json(
                frame.turn ? frame.turn->usage.aggregate : TokenUsage{});
            frame.idle["usage"] = usage;
            frame.done["turn_id"] = frame.turn_id;
            frame.done["usage"] = usage;
        }
        break;
    }
    case Step::Trajectory:
        frame.trajectory.with([&](TrajectoryRecorder& recorder) {
            recorder.record_terminal(frame.idle, frame.done);
        });
        break;
    case Step::BusyCallback:
        if (callbacks.on_busy_changed) callbacks.on_busy_changed(false);
        break;
    case Step::CloseNormal: {
        const auto dropped = gate_.close_and_discard();
        if (dropped > 0) {
            LOG_WARN("[turn/steer] discarded " + std::to_string(dropped) +
                " uncommitted input(s) while closing turn " + frame.turn->info.active_turn_id);
        }
        break;
    }
    case Step::RecordOutcome: outcome_.record(status); break;
    case Step::ClearBusy: busy_ = false; break;
    case Step::BusyEvent: events_.emit(SessionEventKind::BusyChanged, frame.idle); break;
    case Step::Done: events_.emit(SessionEventKind::Done, frame.done); break;
    case Step::AfterNormal: after_normal(*frame.turn); break;
    case Step::ContinueHook: continue_goal(); break;
    }
}

void TurnFinalizer::prepare_normal(Frame& frame) {
    auto& turn = *frame.turn;
    const auto& turn_info = turn.info;
    auto& turn_timing_status = turn.timing_status;
    const auto total_iterations = turn.total_iterations;
    const auto terminator_fired = turn.terminator_fired;
    const int max_iter = frame.max_iterations;
    const bool has_max_iterations = max_iter > 0;
    auto& desktop_turn_lease = *turn.desktop_lease;
    // Post-loop cleanup
    if (!abort_signal_.raw() && !terminator_fired &&
        has_max_iterations && total_iterations >= max_iter) {
        std::string stop_msg = "Agent loop stopped: reached max_iterations (" +
                               std::to_string(max_iter) + ")";
        LOG_WARN(stop_msg);
        turn_timing_status = "error";
        message("system", stop_msg, false,
            make_system_notice_metadata("iteration_limit", {{"limit", max_iter}}));
        {
            // 走的是 system 角色,dispatch_message 的 error 收集点抓不到;
            // 子会话被 cap 截断时父会话同样要拿到原因。
            outcome_.set_error(stop_msg);
        }
    }

    const bool interrupted_for_new_turn =
        abort_signal_.raw().load() && turn_interrupt_requested_.exchange(false);
    if (abort_signal_.raw()) {
        turn_timing_status = "aborted";
        goal_.account_usage(session_manager_, 0, false);
        if (interrupted_for_new_turn) {
            transcript_.append_interrupted_turn_context(session_manager_, turn_info.active_turn_id);
        } else if (session_manager_) {
            const std::string sid = session_manager_->current_session_id();
            ThreadGoalStore* store = session_manager_->goal_store();
            if (store && !sid.empty()) {
                std::string error;
                if (store->pause_active_thread_goal(sid, &error)) {
                    auto goal = store->get_thread_goal(sid);
                    if (goal.has_value()) goal_.emit_updated(*goal);
                }
            }
        }
    } else {
        goal_.account_usage(session_manager_, 0, false);
    }

    if (turn_info.visible_timed_turn && session_manager_) {
        auto turn_diff = session_manager_->finalize_user_turn_net_diff(
            turn_info.turn_user_uuid);
        if (turn_diff.has_value()) {
            events_.emit(SessionEventKind::TurnDiff,
                         encode_turn_net_diff(*turn_diff));
        }
    }

    if (turn_info.visible_timed_turn) {
        transcript_.append_turn_timing_record(session_manager_,
            turn_info.turn_user_uuid, turn_info.turn_started_at_ms, now_epoch_ms(),
            turn_timing_status);
    }

    if (abort_signal_.raw()) {
        if (interrupted_for_new_turn) {
            message("system", "[Interjected]", false,
                make_system_notice_metadata("turn_interjected", {}, {{"turn_interrupt", true}}));
        } else {
            const auto* user = trailing_transcript_message(history_.view(), true);
            // Persist the completed stop, including its exact retry target.
            // This notice stays out of the provider's message history.
            transcript_.emit_transcript_system_message(session_manager_, "[Interrupted]", make_system_notice_metadata("turn_interrupted", {}, {
                {"user_aborted", true},
                {"retry_user_message_id", user ? user->uuid : std::string{}},
            }));
        }
    }

    // 回合结束:阶段前言不留到下一回合。
    activity_.reset_turn();
    desktop_turn_lease.release_before_terminal();

}

void TurnFinalizer::after_normal(TurnContext& turn) {
    if (turn.tools.terminate_session_after_turn) {
        // There must be no provider-visible state left for a deleted session.
        // The post-turn action owns writer teardown and persistent cleanup.
        history_.clear();
        auto actions = std::move(turn.tools.post_turn_actions);
        turn.tools.post_turn_actions.clear();
        for (auto& action : actions) {
            if (!action) continue;
            try {
                action();
            } catch (const std::exception& e) {
                LOG_ERROR(std::string("Post-turn terminal action failed: ") +
                          e.what());
            } catch (...) {
                LOG_ERROR("Post-turn terminal action failed with unknown exception");
            }
        }
    } else {
        continue_goal();
    }
}
void TurnFinalizer::continue_goal() {
    goal_.maybe_continue(session_manager_, {
        tools_.is_allowed("update_goal", &policy_),
        tools_.is_allowed("AskUserQuestion", &policy_)});
}
void TurnFinalizer::message(const std::string& role, const std::string& text,
    bool is_tool, nlohmann::json metadata) {
    transcript_.dispatch_message(role, text, is_tool, std::move(metadata), nlohmann::json::array());
}
} // namespace acecode::agent
