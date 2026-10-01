#pragma once

#include "session/session_client.hpp"
#include "llm/tool_result.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace acecode {

constexpr std::size_t kMaxSideChatHistoryMessages = 200;
constexpr std::size_t kMaxSideChatHistoryBytes = 256 * 1024;
// 侧边对话的只读工具循环上限:工具轮数、每轮调用数、单个结果回填给模型的字节数,
// 以及模型把调用写成正文后最多纠正几次。超出轮数时回填「已达上限」再给一轮作答。
constexpr int kMaxSideChatToolRounds = 8;
constexpr std::size_t kMaxSideChatToolCallsPerRound = 8;
constexpr std::size_t kMaxSideChatToolOutputBytes = 64 * 1024;
constexpr int kMaxSideChatTextToolCallCorrections = 2;

struct SideChatMessage {
    std::string role;
    std::string content;
};

// 侧边对话可用的只读工具。definitions 是发给模型的工具表(模型侧名);execute
// 收到模型原样给出的调用,由提供方负责白名单、权限规则与路径校验
// (AgentLoop::side_chat_toolset)。为空 = 不带工具,模型请求工具即失败。
struct SideChatToolset {
    std::vector<ToolDef> definitions;
    std::function<ToolResult(const ToolCall& call, const std::atomic<bool>* abort_flag)> execute;
    // 模型侧名 → 原生名,只用于界面展示;为空时原样使用模型侧名。
    std::function<std::string(const std::string& model_name)> native_name;
    bool enabled() const { return !definitions.empty() && static_cast<bool>(execute); }
};

// 一次工具调用的进度:先 running,执行完 success / error。target 是路径、
// 搜索模式之类的简短参数,只给界面展示。
struct SideChatToolEvent {
    std::string call_id;
    std::string name;
    std::string target;
    std::string status;
};
using SideChatToolCallback = std::function<void(const SideChatToolEvent&)>;

struct SideChatResult {
    SideQuestionResult response;
    bool cancelled = false;
    std::string code;
};

// This cancellation belongs only to one detached request. Waking the provider
// also interrupts its retry backoff; it never sets the main AgentLoop abort.
class SideChatCancellation {
public:
    void cancel();
    void bind_provider(const std::shared_ptr<LlmProvider>& provider);
    std::atomic<bool> aborted{false};

private:
    std::mutex mutex_;
    std::weak_ptr<LlmProvider> provider_;
};

// reset=true discards the provisional text of the current model step (an
// upstream retry, or a step that wrote a tool call as plain text). Text that
// preceded the step's last tool call stays.
using SideChatStreamCallback = std::function<void(const std::string& delta, bool reset)>;

std::string validate_side_chat_history(const std::vector<SideChatMessage>& history);

// One-line prefix for a one-turn answer listing the read-only tools it used,
// e.g. "(read-only tools: file_read src/a.cpp; grep TODO)\n"; empty if none.
std::string side_question_tools_note(const SideQuestionResult& result);

// With an enabled toolset the model may call the read-only tools; results are
// fed back and the model continues until it answers in text. answer is the
// visible text of every step, separated by blank lines.
SideChatResult run_side_chat(
    std::shared_ptr<LlmProvider> provider,
    std::vector<ChatMessage> context,
    const std::string& question,
    const std::vector<SideChatMessage>& history,
    SideChatCancellation& cancellation,
    const SideChatStreamCallback& callback,
    const SideChatToolset& tools = {},
    const SideChatToolCallback& on_tool = {});

} // namespace acecode
