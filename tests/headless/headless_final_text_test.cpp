// headless(-p)最终回复取法的单元测试(fix-feedback-0924 第 3 条)。
// 被测:src/headless/headless_final_text.cpp::headless_final_assistant_text。

#include <gtest/gtest.h>

#include "headless/headless_final_text.hpp"
#include "llm/llm_provider.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using acecode::ChatMessage;
using acecode::headless::headless_final_assistant_text;

namespace {

ChatMessage make_message(const std::string& role, const std::string& content) {
    ChatMessage message;
    message.role = role;
    message.content = content;
    return message;
}

ChatMessage make_rejected_assistant(const std::string& content = "") {
    ChatMessage message = make_message("assistant", content);
    message.metadata = nlohmann::json{
        {"text_tool_call_rejected",
         {{"format", "invoke"}, {"reason", "unknown_tool"}}},
    };
    return message;
}

ChatMessage make_hidden_correction() {
    ChatMessage message = make_message("user", "[SYSTEM NOTE] re-issue the call");
    message.metadata = nlohmann::json{
        {"hidden_goal_context", true},
        {"text_tool_call_correction", true},
    };
    return message;
}

} // namespace

// 场景:回合以文本工具调用被拒告终(纠正次数耗尽),末尾被拒的 assistant 内容
// 为空串,更早还有一条正常的叙述回复。
// 期望:返回空串 —— 回合没有完成,不能倒回去拿更早的叙述当答案;headless 据此
// 以退出码 1 结束。
// 回归:旧实现只看 `!content.empty()`,被拒回复落盘为 "\n\n\n" 时被当成最终
// 回复,headless 以退出码 0 输出一段空白。
TEST(HeadlessFinalText, RejectedTailYieldsEmptyFinalText) {
    std::vector<ChatMessage> messages{
        make_message("user", "do it"),
        make_message("assistant", "I will look at the files first."),
        make_rejected_assistant(),
        make_hidden_correction(),
        make_rejected_assistant(),
        make_hidden_correction(),
        make_rejected_assistant("Here is an example:"),
    };
    EXPECT_EQ(headless_final_assistant_text(messages, 0), "");

    // 即使被拒消息本身保留了块前的正文,也不能当作最终回复。
    std::vector<ChatMessage> with_prose{
        make_message("user", "do it"),
        make_rejected_assistant("Let me run it."),
    };
    EXPECT_EQ(headless_final_assistant_text(with_prose, 0), "");
}

// 场景:末尾的 assistant 只有空白(例如旧会话里 provider 藏起标记后留下的
// "\n\n\n"),更早有一条正常回复。
// 期望:跳过纯空白,返回更早的那条正常回复。
TEST(HeadlessFinalText, WhitespaceOnlyAssistantIsSkipped) {
    std::vector<ChatMessage> messages{
        make_message("user", "hi"),
        make_message("assistant", "hello there"),
        make_message("assistant", "\n\n\n"),
    };
    EXPECT_EQ(headless_final_assistant_text(messages, 0), "hello there");

    // baseline 之前的消息不参与(那是上一次 -p 调用的回复)。
    EXPECT_EQ(headless_final_assistant_text(messages, 2), "");
}

// 场景:第 1 次被拒,纠正后模型用原生调用继续,最后给出正常回复。
// 期望:返回最后那条正常回复;被拒消息只是中途插曲,不影响结果。
// 另测:纠正后回合以只有工具调用的 assistant(如 task_complete)收尾时,被拒
// 消息不截断查找,照旧往前找叙述(与没有文本调用时的取法一致)。
TEST(HeadlessFinalText, NormalReplyAfterCorrectionIsReturned) {
    std::vector<ChatMessage> messages{
        make_message("user", "do it"),
        make_rejected_assistant(),
        make_hidden_correction(),
        make_message("assistant", ""),
        make_message("tool", "ok"),
        make_message("assistant", "All done."),
    };
    EXPECT_EQ(headless_final_assistant_text(messages, 0), "All done.");

    std::vector<ChatMessage> tool_only_tail{
        make_message("user", "do it"),
        make_message("assistant", "Starting."),
        make_rejected_assistant(),
        make_hidden_correction(),
        make_message("assistant", ""),
        make_message("tool", "done"),
    };
    EXPECT_EQ(headless_final_assistant_text(tool_only_tail, 0), "Starting.");
}
