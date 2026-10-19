#pragma once
#include "agent/callbacks_slot.hpp"
#include "tool/tool_executor.hpp"
#include "utils/lifetime_token.hpp"

namespace acecode { struct AgentCallbacks; }
namespace acecode::agent {
class WorkspaceBoundary;
class ToolSessionHost;
class TranscriptWriter;

// 工具调用 / 结果伪行(tool_call / tool_result message 事件)的身份:与
// tool_start / tool_end 同一个 call id 与序号。Web 据此把伪行配到结构化工具行上;
// 没有它只能按全局顺序猜,内容相同的两次调用还会被当成同一条消息合并。
nlohmann::json tool_row_metadata(const ToolCall& call, std::size_t tool_index);

class ToolResultPresenter {
public:
    ToolResultPresenter(WorkspaceBoundary& boundary, ToolSessionHost& host,
        TranscriptWriter& transcript, CallbacksSlot& callbacks, SessionManager* session)
        : boundary_(boundary), host_(host), transcript_(transcript),
          callbacks_(callbacks), session_manager_(session) {}
    void materialize_attachments(ToolResult& result);
    void display(const ToolCall& call, const ToolResult& result, std::size_t tool_index);
private:
    WorkspaceBoundary& boundary_;
    ToolSessionHost& host_;
    TranscriptWriter& transcript_;
    CallbacksSlot& callbacks_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
    LifetimeToken lifetime_;
};
} // namespace acecode::agent
