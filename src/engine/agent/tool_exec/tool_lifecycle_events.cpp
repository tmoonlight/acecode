#include "tool_lifecycle_events.hpp"
#include "agent/event_payload/tool_event_payload.hpp"
#include "agent/callbacks_slot.hpp"
#include "agent/event_payload/message_payload.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "utils/time.hpp"

namespace acecode::agent {
using utils::now_epoch_ms;

void ToolLifecycleEvents::start(
    const ToolCall& tc, int tool_index_int, const ToolPreambleTitle& step_preamble,
    const std::string& cmd_preview, const std::string& display_override,
    std::int64_t tool_started_at_ms) {
    const bool is_task_complete = tc.function_name == "task_complete";
    const auto& call_preamble = step_preamble.title;
    {
        nlohmann::json args_payload;
        try { args_payload = nlohmann::json::parse(tc.function_arguments); }
        catch (...) { args_payload = tc.function_arguments; }
        auto start_payload = web::build_tool_start_payload(
            tc.function_name, args_payload,
            cmd_preview, display_override,
            is_task_complete, tc.id, tool_index_int);
        start_payload["started_at_ms"] = tool_started_at_ms;
        // 工具前言:这次调用的前言随 tool_start 下发,工具行 / loading 直接用。
        if (!call_preamble.empty()) {
            start_payload["preamble"] = call_preamble;
            start_payload["preamble_source"] = step_preamble.source;
            start_payload["preamble_kind"] = step_preamble.kind;
        }
        events_.emit(
            SessionEventKind::ToolStart, std::move(start_payload));
    }

}

void ToolLifecycleEvents::finish(
    const ToolCall& tc, int tool_index_int, const ToolResult& result,
    std::chrono::steady_clock::time_point tool_start_tp,
    std::int64_t tool_started_at_ms, ToolCallOutcome& outcome) {
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tool_start_tp).count();
    const std::int64_t tool_completed_at_ms = now_epoch_ms();
    std::string snippet;
    if (!result.success) {
        int lines = 0;
        for (char c : result.output) {
            snippet.push_back(c);
            if (c == '\n' && ++lines >= 20) break;
        }
    }
    const bool defer_task_complete_end = tc.function_name == "task_complete" && result.success;
    if (defer_task_complete_end) {
        auto& deferred = outcome.deferred_end;
        deferred.started_at_ms = tool_started_at_ms;
        deferred.completed_at_ms = tool_completed_at_ms;
        deferred.duration_ms = elapsed_ms;
        deferred.elapsed_seconds = elapsed_ms / 1000.0;
    }
    if (session_manager_ && !defer_task_complete_end) {
        auto trajectory_payload = web::build_tool_end_payload(
            tc.function_name, result,
            elapsed_ms / 1000.0,
            result.output,
            tc.id, tool_index_int);
        trajectory_payload["started_at_ms"] = tool_started_at_ms;
        trajectory_payload["completed_at_ms"] = tool_completed_at_ms;
        trajectory_payload["duration_ms"] = elapsed_ms;
        session_manager_->record_trajectory_event(
            "tool_end", std::move(trajectory_payload),
            tool_completed_at_ms);
    }
    if (defer_task_complete_end) {
        // task_complete 的 fork 边界必须指向预算替换后实际落盘的
        // canonical tool-result。延迟 trajectory/live ToolEnd 到 Phase 3
        // 持久化之后,避免超长 summary 的预计算 ID 与 REST/fork 不一致。
    } else {
        events_.emit(
            SessionEventKind::ToolEnd,
            web::build_tool_end_payload(
                tc.function_name, result,
                elapsed_ms / 1000.0, snippet,
                tc.id, tool_index_int));
    }

}

struct ToolLifecycleEvents::Stream::State {
    State(EventDispatcher& events, CallbacksSlot& callbacks, const ToolCall& call,
        int index, bool emit_tui, Clock clock, std::chrono::steady_clock::time_point start)
        : events_(events), callback_(emit_tui ? callbacks.snapshot().on_tool_progress_update : Update{}),
          name_(call.function_name), id_(call.id), index_(index),
          key_("tool_update:" + (!call.id.empty() ? call.id
              : (call.function_name + ":" + std::to_string(index)))),
          clock_(std::move(clock)), start_(start) {}
    void append(const std::string& chunk);
    EventDispatcher& events_;
    Update callback_;
    std::string name_, id_;
    int index_;
    std::string key_;
    Clock clock_;
    std::chrono::steady_clock::time_point start_;
    ToolStreamProgress progress_;
    LifetimeToken lifetime_;
};

ToolLifecycleEvents::Stream::Stream(
    EventDispatcher& events, CallbacksSlot& callbacks, const ToolCall& call,
    int index, bool emit_tui, Clock clock, std::chrono::steady_clock::time_point start)
    : state_(std::make_shared<State>(events, callbacks, call, index, emit_tui, std::move(clock), start)) {}
ToolLifecycleEvents::Stream::~Stream() { state_->lifetime_.revoke(); }

void ToolLifecycleEvents::Stream::bind(ToolContext& context) {
    context.stream = [weak = std::weak_ptr<State>(state_)](const std::string& chunk) {
        if (auto active = weak.lock()) {
            active->lifetime_.ref(*active).with([&](State& state) { state.append(chunk); });
        }
    };
}

void ToolLifecycleEvents::Stream::State::append(const std::string& chunk) {
    const auto progress = progress_.append(
        chunk, clock_ ? clock_() : std::chrono::steady_clock::now());
    if (callback_) {
        callback_(progress.tail_lines, progress.current_partial,
            progress.total_bytes, progress.total_lines);
    }
    if (!progress.should_emit) return;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_).count();
    EventDispatcher::EmitOptions options;
    options.buffered = true;
    options.coalesce_key = key_;
    events_.emit(SessionEventKind::ToolUpdate,
        web::build_tool_update_payload(name_, progress.tail_lines,
            progress.current_partial, progress.total_lines, progress.total_bytes,
            elapsed_ms / 1000.0, id_, index_), options);
}

void ToolLifecycleEvents::finish_deferred(
    const ToolCall& tc, int index, const ToolCallOutcome& outcome, const ChatMessage& tool_msg) {
    const auto& deferred = outcome.deferred_end;
    auto end_payload = web::build_tool_end_payload(
        tc.function_name, outcome.result, deferred.elapsed_seconds,
        outcome.result.output, tc.id, index,
        web::compute_message_id(tool_msg));
    if (session_manager_) {
        auto trajectory_payload = end_payload;
        trajectory_payload["started_at_ms"] = deferred.started_at_ms;
        trajectory_payload["completed_at_ms"] = deferred.completed_at_ms;
        trajectory_payload["duration_ms"] = deferred.duration_ms;
        session_manager_->record_trajectory_event(
            "tool_end", std::move(trajectory_payload),
            deferred.completed_at_ms);
    }
    events_.emit(
        SessionEventKind::ToolEnd, std::move(end_payload));
}
} // namespace acecode::agent
