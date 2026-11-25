#pragma once

// 自动模式（试验）的提示词(openspec add-auto-review-mode,design D9 / D10)。
//
// 两类文字都在这里,都是常量或只由参数决定,同一版本逐字节稳定:
//   1. 给裁决模型的系统提示(默认放行 + 两张拦截表 + 例外 + 用户意图规则)与两个
//      阶段的指令。措辞是 ACECode 自己写的,只借用 Claude Code auto 模式的机制,
//      按本地开发场景裁剪到几千 token。
//   2. 给主模型的文字:模式打开时注入的一段说明,以及裁决拦下 / 没有结果时的
//      工具结果正文。
// 改动这里的任何一句都会让 provider 侧的缓存失效一次,属于正常代价;但不要引入
// 随时间或环境变化的内容(时间戳、cwd、计数),那会让每次裁决都失去缓存。

#include "llm/llm_provider.hpp"

#include <string>
#include <vector>

namespace acecode::auto_review {

enum class ReviewStage {
    Quick,   // 第一阶段:只看操作本身的后果,宁可错拦,只答拦不拦
    Full     // 第二阶段:结合例外与用户意图复核,拦下时给一句理由
};

// 裁决模型的系统提示(常量)。
const std::string& reviewer_system_prompt();

// 某个阶段的收尾指令(常量)。
const std::string& stage_instruction(ReviewStage stage);

// 一次裁决请求的消息:system + 一条 user(记录、待裁决的调用、阶段指令)。
// transcript / action 分别是 render_transcript / render_action 的输出。
std::vector<ChatMessage> build_review_messages(const std::string& transcript,
                                               const std::string& action,
                                               ReviewStage stage);

// 模式打开期间给主模型的说明(进请求上下文的 auto_review 分节)。
const std::string& main_model_guidance();

// 以下是回给主模型的工具结果正文。tool 传模型侧工具名(调用方经
// model_tool_name_for_native 取),不要传原生名。
std::string blocked_result_text(const std::string& tool, const std::string& reason);
std::string unavailable_result_text(const std::string& tool, const std::string& why);
std::string locked_result_text(const std::string& tool);
std::string needs_user_result_text(const std::string& tool);

} // namespace acecode::auto_review
