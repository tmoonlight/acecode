#pragma once
#include "agent/callbacks_slot.hpp"

#include "pa/pa_rescue_driver.hpp"
#include "agent/model_step/active_model_view.hpp"
#include <utility>

namespace acecode { struct AgentCallbacks; class EventDispatcher; class AbortSignal; }
namespace acecode::agent {
class ConversationHistory;
class TranscriptWriter;
class CompactionController;
class RetryProgressReporter;
struct CompactionInputs;

// 把历史里的内联图片换成文件引用(ThreadRepairOptions::withhold_inline_images),
// 成功时标记历史已修复并发一条用户可见通知。返回撤下的图片数,0 = 没有可撤的
// 图片或修复失败。PA 兜底与通用超限恢复链共用这一处,文案只有一份。
int withhold_inline_images_for_retry(ConversationHistory& history,
                                     TranscriptWriter& transcript,
                                     CompactionController& compaction,
                                     SessionManager* session);

// 服务端以上下文超限拒收后先做一次自动压缩(CompactionController::run_auto:
// 摘要压缩,摘要失败时退到机械修剪),压缩通知由它自己发。PA 兜底与通用超限恢复
// 链共用。inputs 为空(调用方没给压缩入参)按 Failed 处理,直接走原来的收缩链。
pa::CompactOutcome compact_history_for_overflow(CompactionController& compaction,
                                                const CompactionInputs* inputs,
                                                AbortSignal& abort);

// Invocation-scoped adapter, never implemented by or pointed back at AgentLoop.
// Its fixed nullable session borrow cannot escape the synchronous driver call.
class PaRescueAdapter final : public pa::PaRescueHost {
public:
    PaRescueAdapter(ConversationHistory& history, TranscriptWriter& transcript,
        CompactionController& compaction, RetryProgressReporter& retry,
        CallbacksSlot& callbacks, EventDispatcher& events, AbortSignal& abort,
        SessionManager* session, ActiveModelView model,
        const CompactionInputs* compaction_inputs = nullptr)
        : history_(history), transcript_(transcript), compaction_(compaction), retry_(retry),
          callbacks_(callbacks), events_(events), abort_(abort), session_(session),
          model_(std::move(model)), compaction_inputs_(compaction_inputs) {}
    int history_tokens() const override;
    void note_rejection(int request_tokens) override;
    ThreadRepairResult repair(const ThreadRepairOptions& options) override;
    int withhold_inline_images() override;
    pa::CompactOutcome compact() override;
    bool wait(int milliseconds) override;
    void reset_stream() override;
    void history_repaired() override;
    void notice(const std::string& text, nlohmann::json metadata) override;
    void progress(nlohmann::json payload) override;
    void retry(const ProviderErrorInfo& info, bool waiting,
               std::string label, std::string detail) override;
private:
    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    CompactionController& compaction_;
    RetryProgressReporter& retry_;
    CallbacksSlot& callbacks_;
    EventDispatcher& events_;
    AbortSignal& abort_;
    SessionManager* const session_; // nullable, borrowed until run_rescue returns
    ActiveModelView model_;
    const CompactionInputs* const compaction_inputs_; // nullable, same borrow scope
};
} // namespace acecode::agent
