#include "message_predicates.hpp"

namespace acecode {

namespace {

const std::string kSummaryPrefix =
    "Another language model started to solve this problem and produced a summary of its thinking process. You also have access to the state of the tools that were used by that language model. Use this to build on the work that has already been done and avoid duplicating work. Here is the summary produced by the other language model, use the information in this summary to assist with your own analysis:";

bool has_internal_user_context_metadata(const ChatMessage& msg) {
    if (!msg.metadata.is_object()) return false;

    static constexpr const char* kInternalContextKeys[] = {
        "transcript_only",
        "hidden_goal_context",
        "hidden_plan_mode_context",
        "hidden_todo_context",
        "hidden_hook_stop_continuation",
        "compact_initial_context",
    };
    for (const char* key : kInternalContextKeys) {
        if (msg.metadata.value(key, false)) return true;
    }
    // 蜂群模式（网状）的跨 agent 信封(session/inter_agent_message.hpp):MESSAGE /
    // FINAL_ANSWER 属内部上下文;NEW_TASK 是子 agent 收到的任务,等同真实用户请求,
    // 可变上下文锚在它前面,回合中途到达的其它信封不会挪动锚点打穿前缀缓存。
    const auto envelope = msg.metadata.find("inter_agent");
    if (envelope != msg.metadata.end() && envelope->is_object()) {
        return envelope->value("type", std::string{}) != "NEW_TASK";
    }
    return false;
}

} // namespace

const std::string& get_compact_summary_prefix() {
    return kSummaryPrefix;
}

bool is_compact_summary_message(const ChatMessage& msg) {
    const std::string prefix = get_compact_summary_prefix() + "\n";
    return msg.is_compact_summary || msg.content.rfind(prefix, 0) == 0;
}

bool is_real_user_message(const ChatMessage& msg) {
    if (msg.role != "user" || msg.is_meta) return false;
    if (has_internal_user_context_metadata(msg)) return false;
    return !is_compact_summary_message(msg);
}

} // namespace acecode
