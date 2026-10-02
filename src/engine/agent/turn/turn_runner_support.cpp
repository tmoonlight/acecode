#include "turn_runner.hpp"
#include "turn_context.hpp"
#include "active_turn_gate.hpp"
#include "agent/mailbox/agent_mailbox.hpp"
#include "agent/compaction/compaction_controller.hpp"
#include "agent/request/request_context_source.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include <utility>

namespace acecode::agent {
TurnRunner::TurnRunner(TurnRunnerServices s, TurnRunnerOptions options)
    : tools_(s.tools.tools), callbacks_(s.tools.callbacks), permissions_(s.tools.permissions),
      history_(s.tools.history), transcript_(s.tools.transcript), hooks_(s.tools.hooks),
      tool_hooks_(s.tools.tool_hooks), boundary_(s.tools.boundary), security_(s.tools.security),
      prompt_cache_(s.tools.prompt_cache), goal_(s.tools.goal), events_(s.tools.events),
      abort_(s.tools.abort), config_(s.tools.config), source_(s.request_source),
      session_(s.tools.session), hook_manager_(s.tools.hook_manager),
      permission_prompter_(s.tools.permission_prompter), question_prompter_(s.tools.question_prompter),
      outcome_(s.outcome), gate_(s.gate), mailbox_(s.mailbox), activity_(s.activity),
      side_questions_(s.side_questions),
      usage_(s.usage), steps_(s.steps), stream_(s.stream), compaction_(s.compaction),
      recovery_(s.recovery), busy_(s.busy), interrupt_(s.interrupt), context_window_(s.context_window),
      suggestion_threshold_(s.suggestion_threshold), options_(std::move(options)),
      requests_(boundary_, security_, source_, context_window_, session_, tools_,
                permissions_, history_, s.requests, hooks_),
      finalizer_({history_, transcript_, outcome_, gate_, goal_, hooks_, activity_,
                  events_, callbacks_, tools_, source_.tool_policy, busy_, abort_, interrupt_, session_}) {}

CompactionInputs TurnRunner::compaction_inputs(
    const TurnContext& turn, LifetimeRef<TrajectoryRecorder> terminal) const {
    CompactionInputs inputs;
    inputs.session = session_;
    inputs.hooks = hook_manager_;
    inputs.provider = options_.provider ? options_.provider() : nullptr;
    inputs.request = requests_.options(inputs.provider);
    inputs.suggestion_threshold = suggestion_threshold_.load(std::memory_order_relaxed);
    inputs.terminal = terminal;
    return inputs;
}

bool TurnRunner::drain_inputs(bool close_if_empty) {
    auto drained = gate_.drain(close_if_empty);
    for (auto& input : drained.inputs) {
        transcript_.commit_turn_steering_input(session_, std::move(input), drained.turn_id);
    }
    bool any = !drained.inputs.empty();
    // Codex get_pending_input: steering first, then the session mailbox. After
    // a final answer queue-only mail waits for the next turn unless same-turn
    // user input keeps this turn open; trigger mail already queued a wake turn.
    if (!close_if_empty || any) {
        for (auto& mail : mailbox_.take_all()) {
            transcript_.commit_inter_agent_message(session_, std::move(mail.input));
            any = true;
        }
    }
    return any;
}

ToolBatchOutcome TurnRunner::execute_tools(TurnContext& turn, const ChatResponse& response,
    const std::shared_ptr<LlmProvider>& provider, const ProgressEmitter& progress) {
    auto pipeline = std::make_unique<ToolBatchScheduler>(
        ToolExecutionServices{tools_, callbacks_, permissions_, history_, transcript_, hooks_,
            tool_hooks_, boundary_, security_, prompt_cache_, goal_, events_, abort_, config_,
            source_.tool_policy, session_, hook_manager_, source_.skills.get(),
            permission_prompter_, question_prompter_},
        ToolExecutionOptions{options_.provider, options_.question_channel,
            turn.progress_clock, turn.model_tool_names});
    auto outcome = pipeline->execute(response, provider, progress, turn.doom_guard, turn.preamble);
    for (auto& action : outcome.post_turn_actions) {
        if (!action) continue;
        action = [ref = turn.callback_lifetime.ref(turn), owned = std::move(action)] {
            ref.with([&](TurnContext&) { owned(); });
        };
    }
    return outcome;
}
} // namespace acecode::agent
