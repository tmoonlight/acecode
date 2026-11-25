#include "auto_review_gate.hpp"

#include "agent/approval/session_exec_security.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "permissions/auto_review_prompt.hpp"
#include "permissions/auto_review_transcript.hpp"
#include "permissions/auto_review_verdict.hpp"
#include "session/session_manager.hpp"
#include "utils/abandonable_call.hpp"
#include "utils/abort_signal.hpp"
#include "utils/joining_thread.hpp"
#include "utils/logger.hpp"

#include <utility>
#include <vector>

namespace acecode::agent {
namespace {

using auto_review::ParsedVerdict;
using auto_review::ReviewStage;

struct StageReply {
    enum class Status { Ok, Failed, TimedOut, Aborted };
    Status status = Status::Failed;
    std::string text;    // Ok 时是模型回答的正文
    std::string error;   // Failed 时的原因简述
};

std::int64_t elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - since).count();
}

// 发一次不带工具的非流式请求。走普通对话路径(for_compaction 为假):摘要压缩
// 那条路在 PA 适配里带「被拒按超限收缩」的学习,裁决请求失败不能记进那张表。
StageReply ask_stage(const std::shared_ptr<LlmProvider>& provider, std::vector<ChatMessage> messages,
                     const std::string& prompt_cache_key, std::chrono::milliseconds budget,
                     const std::atomic<bool>& user_abort) {
    // cancelled 由看门线程在「用户中断」或「到时」时置位;请求线程和 provider 都只看它。
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto timed_out = std::make_shared<std::atomic<bool>>(false);
    const std::atomic<bool>* abort_flag = &user_abort;   // 看门线程在本函数返回前 join,借用安全。
    JoiningThread watchdog([cancelled, timed_out, abort_flag, budget](StopToken stop) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (!stop.wait_for(std::chrono::milliseconds(25))) {
            if (abort_flag->load(std::memory_order_acquire)) {
                cancelled->store(true, std::memory_order_release);
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out->store(true, std::memory_order_release);
                cancelled->store(true, std::memory_order_release);
                return;
            }
        }
    });

    ChatRequestOptions options;
    options.prompt_cache_key = prompt_cache_key;
    options.for_compaction = false;
    StageReply reply;
    try {
        auto response = run_abandonable<ChatResponse>(
            [provider, owned = std::move(messages), options, cancelled] {
                return provider->chat_with_options(owned, {}, options, cancelled.get());
            },
            cancelled.get());
        watchdog.request_stop();
        if (user_abort.load(std::memory_order_acquire)) {
            reply.status = StageReply::Status::Aborted;
        } else if (timed_out->load(std::memory_order_acquire) || !response) {
            reply.status = StageReply::Status::TimedOut;
        } else if (response->finish_reason == "error" || response->provider_error.has_error()) {
            reply.status = StageReply::Status::Failed;
            reply.error = response->provider_error.status_code > 0
                ? "HTTP " + std::to_string(response->provider_error.status_code)
                : std::string("request failed");
        } else {
            reply.status = StageReply::Status::Ok;
            reply.text = std::move(response->content);
        }
    } catch (const std::exception& e) {
        watchdog.request_stop();
        LOG_WARN(std::string("[auto-review] reviewer request threw: ") + e.what());
        reply.status = StageReply::Status::Failed;
        reply.error = "request failed";
    }
    return reply;
}

} // namespace

ReviewOutcome run_two_stage_review(const std::shared_ptr<LlmProvider>& provider,
                                   const std::string& transcript,
                                   const std::string& action,
                                   const std::string& prompt_cache_key,
                                   const std::atomic<bool>& user_abort,
                                   const ReviewTimeouts& timeouts) {
    const auto started = std::chrono::steady_clock::now();
    ReviewOutcome outcome;
    if (!provider) {
        outcome.reason = "no model is available for the review";
        return outcome;
    }
    outcome.model = provider->model();
    if (!provider->supports_tool_free_chat()) {
        outcome.reason = "the reviewer model cannot answer without tools";
        return outcome;
    }

    const auto no_verdict = [&](StageReply::Status status, const std::string& error, const char* stage) {
        outcome.stage = stage;
        outcome.duration_ms = elapsed_ms(started);
        if (status == StageReply::Status::Aborted) {
            outcome.kind = ReviewOutcome::Kind::Aborted;
        } else {
            outcome.kind = ReviewOutcome::Kind::NoVerdict;
            outcome.reason = status == StageReply::Status::TimedOut ? "timeout" : error;
        }
        return outcome;
    };

    // 第一阶段:只看后果,宁可错拦。答不拦就放行;请求失败或超时直接算没有结果
    // (服务都够不着时再问一遍没有意义);答拦或没按格式答才进第二阶段。
    const StageReply quick = ask_stage(provider,
        auto_review::build_review_messages(transcript, action, ReviewStage::Quick),
        prompt_cache_key, timeouts.quick, user_abort);
    if (quick.status != StageReply::Status::Ok) return no_verdict(quick.status, quick.error, "review_stage1");
    if (auto_review::parse_verdict(quick.text).kind == ParsedVerdict::Kind::Allow) {
        outcome.kind = ReviewOutcome::Kind::Allowed;
        outcome.stage = "review_stage1";
        outcome.duration_ms = elapsed_ms(started);
        return outcome;
    }

    // 第二阶段:结合例外与用户意图复核,结论以它为准。
    const StageReply full = ask_stage(provider,
        auto_review::build_review_messages(transcript, action, ReviewStage::Full),
        prompt_cache_key, timeouts.full, user_abort);
    if (full.status != StageReply::Status::Ok) return no_verdict(full.status, full.error, "review_stage2");
    const ParsedVerdict verdict = auto_review::parse_verdict(full.text);
    outcome.stage = "review_stage2";
    outcome.duration_ms = elapsed_ms(started);
    switch (verdict.kind) {
        case ParsedVerdict::Kind::Allow:
            outcome.kind = ReviewOutcome::Kind::Allowed;
            break;
        case ParsedVerdict::Kind::Block:
            outcome.kind = ReviewOutcome::Kind::Blocked;
            outcome.reason = verdict.reason;
            break;
        case ParsedVerdict::Kind::Invalid:
            // 格式错误不是拦截:记成没有结果,不进拦截计数。
            outcome.kind = ReviewOutcome::Kind::NoVerdict;
            outcome.reason = "the reviewer's answer could not be read";
            break;
    }
    return outcome;
}

ReviewOutcome AutoReviewGate::review(const ToolCall& call, const std::string& execution) const {
    // 配置的裁决模型优先;没配置、名字已不存在或建不起连接时用会话当前的模型。
    std::shared_ptr<LlmProvider> provider = security_.review_provider();
    if (!provider && session_provider_) provider = session_provider_();

    auto_review::ReviewAction action;
    action.tool_name = call.function_name;
    action.arguments_json = call.function_arguments;
    action.tool_call_id = call.id;
    action.execution = execution;
    auto_review::TranscriptOptions options;
    options.group_delegate = group_delegate_;

    // 裁决的缓存键与主对话分开,互不挤占 provider 侧的会话路由。
    const std::string session_id = session_ ? session_->current_session_id() : std::string{};
    ReviewOutcome outcome = run_two_stage_review(provider,
        auto_review::render_transcript(history_.view(), action, options),
        auto_review::render_action(action, options.limits),
        session_id.empty() ? std::string{} : session_id + ":auto-review",
        abort_.raw(), security_.review_timeouts());
    LOG_INFO("[auto-review] " + call.function_name + " -> " +
             (outcome.kind == ReviewOutcome::Kind::Allowed ? "allowed"
              : outcome.kind == ReviewOutcome::Kind::Blocked ? "blocked"
              : outcome.kind == ReviewOutcome::Kind::Aborted ? "aborted" : "no verdict") +
             " stage=" + outcome.stage + " model=" + outcome.model +
             " ms=" + std::to_string(outcome.duration_ms) +
             (outcome.reason.empty() ? std::string{} : " reason=" + outcome.reason));
    return outcome;
}

std::string AutoReviewGate::user_turn_anchor() const {
    return auto_review::last_user_authored_id(history_.view());
}

} // namespace acecode::agent
