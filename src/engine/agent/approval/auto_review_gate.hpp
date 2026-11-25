#pragma once

// 自动模式（试验）的裁决编排(openspec add-auto-review-mode,design D6 / D8)。
//
// 只负责「问一次裁决模型」:选 provider(配置的裁决模型,否则会话当前模型)、渲染
// 记录、两阶段调用、每阶段时限、响应回合中断,把结果归成 放行 / 拦下 / 没有结果。
// 这里不做任何审批决定,也不碰计数与审计 —— 那些留在唯一的审批入口
// ToolPermissionGate::decide 里。

#include "llm/llm_provider.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace acecode { class AbortSignal; class SessionManager; }

namespace acecode::agent {
class ConversationHistory;
class SessionExecSecurity;

struct ReviewOutcome {
    enum class Kind {
        Allowed,    // 裁决放行
        Blocked,    // 裁决拦下,reason 是给主模型和审计看的一句话
        NoVerdict,  // 请求出错 / 超时 / 解析不出 / 模型不可用;reason 是原因简述
        Aborted     // 用户中断了回合
    };
    Kind kind = Kind::NoVerdict;
    std::string reason;
    std::string stage;   // 审计原因:review_stage1 / review_stage2
    std::string model;   // 裁决模型的标识
    std::int64_t duration_ms = 0;
};

struct ReviewTimeouts {
    // 第一阶段只要一个标记,20 秒足够;第二阶段允许简短思考,给 45 秒。超时算
    // 「没有结果」,不算拦下。
    std::chrono::milliseconds quick{20000};
    std::chrono::milliseconds full{45000};
};

// 两阶段裁决本身(可单测):第一阶段答不拦就放行;答拦或解析不出才进第二阶段。
// provider 以 shared_ptr 传入:请求在可放弃的后台调用里执行,超时后调用方先返回,
// 那次调用自己持有 provider 直到结束。
ReviewOutcome run_two_stage_review(const std::shared_ptr<LlmProvider>& provider,
                                   const std::string& transcript,
                                   const std::string& action,
                                   const std::string& prompt_cache_key,
                                   const std::atomic<bool>& user_abort,
                                   const ReviewTimeouts& timeouts = {});

class AutoReviewGate {
public:
    using ProviderAccessor = std::function<std::shared_ptr<LlmProvider>()>;

    AutoReviewGate(SessionExecSecurity& security, ConversationHistory& history, AbortSignal& abort,
                   SessionManager* session, ProviderAccessor session_provider, bool group_delegate)
        : security_(security), history_(history), abort_(abort), session_(session),
          session_provider_(std::move(session_provider)), group_delegate_(group_delegate) {}

    // execution:一句话说明这次调用将以什么边界执行(可为空)。
    ReviewOutcome review(const ToolCall& call, const std::string& execution) const;

    // 最后一条用户亲发消息的标识(锁定用)。
    std::string user_turn_anchor() const;

private:
    SessionExecSecurity& security_;
    ConversationHistory& history_;
    AbortSignal& abort_;
    SessionManager* session_; // Nullable borrowed constructor dependency.
    ProviderAccessor session_provider_;
    bool group_delegate_;
};

} // namespace acecode::agent
