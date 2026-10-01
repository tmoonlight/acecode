#include "compact_prompt.hpp"

#include <string>

namespace acecode {

namespace {

const std::string kCompactPrompt =
    "You are performing a CONTEXT CHECKPOINT COMPACTION. Create a handoff summary for another LLM that will resume the task.\n\n"
    "Include:\n"
    "- Current progress and key decisions made\n"
    "- Important context, constraints, or user preferences\n"
    "- What remains to be done (clear next steps)\n"
    "- Any critical data, examples, or references needed to continue\n"
    // ACECode 追加的四条(反馈 LINDANDAN069):压缩后模型忘了用户的原始要求与
    // 纠正、忘了刚总结出的做法,又把已验证的函数重写一遍、重新踩坑。
    "- Every explicit instruction, requirement and correction the user gave, quoted as closely as possible; they must survive this checkpoint\n"
    "- Lessons learned in this session: what failed, why, and the approach that finally worked\n"
    "- Scripts, helper functions and files that were already written and verified, with exact paths and names, so the next LLM reuses them instead of rewriting them\n"
    "- Skills and memory entries that were loaded and still apply, by name, so the next LLM can reload them\n\n"
    "Be concise, structured, and focused on helping the next LLM seamlessly continue the work."
    // 以下一段是 ACECode 有意偏离 Codex 原文的追加:压缩请求不带工具表,部分模型
    // (实测 dots3)会接着历史里的 tool_calls「做下一步」,把工具调用写成正文当摘要。
    // 只禁止调工具 / 输出调用标签,不禁止引用命令与路径 —— 上面要求保留关键数据。
    "\n\nOutput requirements: tools are not available for this request. Reply with the summary as plain text only; "
    "do not call any tool and do not emit tool-call or function-call tags. "
    "You may still quote commands, paths and code that the next step needs.";

const std::string kInvalidSummaryReminder =
    "Your previous reply was not a valid summary (it contained a tool call or was empty). "
    "Tools are disabled for this request. Write the handoff summary now as plain text.";

} // namespace

const std::string& get_compact_prompt() {
    return kCompactPrompt;
}

const std::string& get_compact_invalid_summary_reminder() {
    return kInvalidSummaryReminder;
}

std::string get_compact_user_summary_message(const std::string& summary_text) {
    return get_compact_summary_prefix() + "\n" + summary_text;
}

} // namespace acecode
