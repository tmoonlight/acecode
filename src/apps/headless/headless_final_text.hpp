#pragma once

// headless(-p)最终回复的取法(fix-feedback-0924 第 3 条)。
//
// 从 headless_runner.cpp 抽出的纯函数,便于单测:回合结束后在 baseline 之后的
// 消息里从后往前找最终 assistant 回复。
//
// 旧实现只要 `!content.empty()` 就认作最终回复,于是模型把工具调用写成正文、
// provider 藏起标记后只剩 "\n\n\n" 的那条 assistant 被当成答案,headless 以
// 退出码 0 输出一段空白。现在:
//   - 内容只有空白的 assistant 消息跳过;
//   - 遇到带 `text_tool_call_rejected` metadata 的 assistant 消息,且它之后再
//     没有任何 assistant 消息(回合以被拒告终,即纠正次数耗尽)→ 返回空,
//     不能倒回去拿更早的叙述当答案;headless 据此以退出码 1 结束。
//     被拒之后回合还继续了(纠正成功,后面有新的 assistant)时,被拒消息
//     只是中途插曲,跳过它照常往前找。

#include "llm/llm_provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace acecode::headless {

std::string headless_final_assistant_text(const std::vector<ChatMessage>& messages,
                                          std::size_t baseline);

} // namespace acecode::headless
