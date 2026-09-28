#include "tool_batch_scheduler.hpp"
#include "agent/guards/doom_guard.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "utils/abort_signal.hpp"
#include "utils/future_join_guard.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <thread>

namespace acecode::agent {

ToolBatchScheduler::ToolBatchScheduler(ToolExecutionServices services, ToolExecutionOptions options)
    : tools_(services.tools), transcript_(services.transcript), goal_(services.goal),
      abort_signal_(services.abort), session_manager_(services.session),
      contexts_(services.boundary, services.security, services.prompt_cache,
          services.permissions, services.goal, services.events, services.abort,
          services.session, services.skills, services.policy, services.config,
          std::move(options.provider)),
      paths_(services.tools, services.permissions, services.boundary, contexts_, services.session),
      exec_(services.security, services.permissions, services.boundary, contexts_,
          services.goal, services.session),
      confirmation_(services.permissions, services.security, contexts_, services.callbacks,
          services.abort, services.session, services.permission_prompter),
      gate_(services.tools, services.permissions, services.boundary, services.security,
          contexts_, services.goal, services.tool_hooks, exec_, paths_, confirmation_,
          services.session, services.hook_manager),
      invoker_(services.tools, paths_, gate_, std::move(options.model_tool_names)),
      presenter_(services.boundary, contexts_, services.transcript, services.callbacks, services.session),
      lifecycle_events_(services.events, services.session),
      questions_(services.goal, services.abort, services.config, services.session,
          services.question_prompter, std::move(options.question_channel)),
      lifecycle_(services.tool_hooks, contexts_, questions_, invoker_, presenter_,
          lifecycle_events_, services.events, services.callbacks, services.hook_manager,
          services.session, std::move(options.clock)),
      message_(services.history, services.hooks, services.events, services.session, services.hook_manager),
      committer_(services.history, services.transcript, lifecycle_events_, services.session, services.security.environment()) {}

ToolBatchOutcome ToolBatchScheduler::execute(
    const ChatResponse& accumulated, const std::shared_ptr<LlmProvider>& provider_snapshot,
    const ProgressEmitter& emit_progress, SynchronizedDoomGuard& doom_guard,
    ToolPreambleTitle& pending_preamble) {
    const auto step_preamble = message_.record(accumulated, provider_snapshot, pending_preamble);
    // Partition tool calls into read-only (parallelizable) and write (serial) groups
    LOG_INFO("Processing " + std::to_string(accumulated.tool_calls.size()) + " tool calls");

    ToolBatchState batch{doom_guard, emit_progress, step_preamble, {}};
    auto& slots = batch.slots;
    slots.reserve(accumulated.tool_calls.size());
    std::vector<std::size_t> read_entries, write_entries;
    for (std::size_t i = 0; i < accumulated.tool_calls.size(); ++i) {
        const auto& tc = accumulated.tool_calls[i];
        slots.push_back({i, tc, std::nullopt});
        if (tools_.can_execute_in_parallel(tc.function_name)) {
            read_entries.push_back(i);
        } else {
            write_entries.push_back(i);
        }
    }

    LOG_INFO("Partitioned: " + std::to_string(read_entries.size()) + " read-only, " +
             std::to_string(write_entries.size()) + " write");

    // Phase 1: Execute read-only tools in parallel
    if (!read_entries.empty() && !abort_signal_.raw()) {
        unsigned int max_concurrency = std::min(
            static_cast<unsigned int>(4),
            std::max(static_cast<unsigned int>(1), std::thread::hardware_concurrency()));

        struct PendingReadTool {
            size_t original_index;
            ToolCall call;
            std::size_t future_index;
        };

        size_t i = 0;
        while (i < read_entries.size() && !abort_signal_.raw()) {
            size_t batch_end = std::min(i + max_concurrency, read_entries.size());
            std::vector<PendingReadTool> pending;
            utils::FutureJoinGuard<agent::ToolCallOutcome> futures;

            for (size_t j = i; j < batch_end; ++j) {
                const auto original_index = read_entries[j];
                ToolCall tc_copy = slots[original_index].call;
                pending.push_back(PendingReadTool{
                    original_index,
                    tc_copy,
                    futures.add(std::async(std::launch::async,
                    [life = lifecycle_.ref(), batch_ref = batch.lifetime.ref(batch),
                     tc_copy, original_index]() {
                        ToolCallOutcome outcome{ToolResult{"[Interrupted]", false}, {}, {}};
                        life.with([&](ToolCallLifecycle& lifecycle) {
                            batch_ref.with([&](ToolBatchState& joined_batch) {
                                outcome = lifecycle.run(
                                    joined_batch, tc_copy, original_index, false);
                            });
                        });
                        return outcome;
                    }))
                });
            }

            for (auto& item : pending) {
                size_t idx = item.original_index;
                // 展示层成对派发:调用行在阻塞等待它的结果之前亮出(执行中
                // 灰色指示灯),结果一到紧跟其后 —— 即使批内并行执行,transcript
                // 仍按提交顺序呈现「调用 → 结果」相邻的成对行。abort 时未收割
                // 的调用不再显示伪行,canonical 的 [Interrupted] 由 Phase 3 落盘。
                transcript_.dispatch_message("tool_call",
                    "[Tool: " + item.call.function_name + "] " +
                        item.call.function_arguments, true, nlohmann::json::object(), nlohmann::json::array());
                try {
                    slots[idx].outcome = futures.get(item.future_index);
                } catch (const std::exception& e) {
                    slots[idx].outcome.emplace();
                    slots[idx].outcome->result =
                        ToolResult{"[Error] " + std::string(e.what()), false};
                    ensure_tool_summary(
                        item.call.function_name,
                        item.call.function_arguments,
                        slots[idx].outcome->result);
                }
                batch.doom_guard.record_result(item.call, slots[idx].outcome->result);
                goal_.account_usage(session_manager_, 0, false);
                presenter_.display(item.call, slots[idx].outcome->result);
            }

            futures.join();
            i = batch_end;
        }
    }

    // Phase 2: Execute write tools sequentially (with permission checks)
    for (const auto& entry : write_entries) {
        if (abort_signal_.raw()) break;

        auto& slot = slots[entry];
        const auto& tc = slot.call;
        LOG_INFO("Tool call (write): " + tc.function_name + " id=" + tc.id);

        transcript_.dispatch_message("tool_call",
                "[Tool: " + tc.function_name + "] " + tc.function_arguments, true, nlohmann::json::object(), nlohmann::json::array());

        slot.outcome = lifecycle_.run(batch, tc, slot.original_index, true);
        batch.doom_guard.record_result(tc, slot.outcome->result);
        goal_.account_usage(session_manager_, 0, false);
        // 结果行紧跟派发。调用行在执行前已显示(权限确认弹窗需要上下文),
        // 写工具串行执行,顺序天然成对。
        presenter_.display(tc, slot.outcome->result);
        if (slot.outcome->result.terminate_session_after_turn) {
            LOG_INFO("Stopping remaining write tools after terminal session action");
            break;
        }
    }

    return committer_.commit(slots);
}

} // namespace acecode::agent
