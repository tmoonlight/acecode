// 覆盖 src/tool/ask_user_question_tool.cpp 的纯函数路径:
//   1. validate_ask_user_question_args 的合法输入 / 非法输入分支(问题数 /
//      选项数 / header 长度 / 问题文本唯一性 / 选项 label 唯一性 / preview
//      字段容忍)
//   2. format_ask_answers 的拼接契约(单题、多题 + multi-select、引号不转义)
//   3. make_rejected_ask_result 的固定拒绝文本
// 另覆盖 TUI 传输层(src/tui/tui_ask_channel.cpp)的 overlay 超时清理、
// 提前回答优先、子代理来源标注与中止路径。

#include <gtest/gtest.h>

#include <ftxui/component/screen_interactive.hpp>

#include "tool/ask_user_question_tool.hpp"
#include "tui/tui_ask_channel.hpp"
#include "tui_state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <string>
#include <thread>
#include <vector>

using acecode::AskQuestion;
using acecode::AskOption;
using acecode::build_ask_user_question_result_metadata;
using acecode::format_ask_answers;
using acecode::format_ask_user_question_result_display;
using acecode::format_single_answer;
using acecode::make_rejected_ask_result;
using acecode::validate_ask_user_question_args;

namespace {

constexpr const char* kInteractiveQuestionArgs = R"({
    "questions": [{
        "question": "Pick one?",
        "header": "choice",
        "options": [
            {"label": "A", "description": "recommended"},
            {"label": "B", "description": "alternative"}
        ]
    }]
})";

bool wait_for_ask_overlay(acecode::TuiState& state,
                          std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.ask_pending) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

// 场景:合法最小输入(1 题 2 选项,均含必填字段)应通过校验,并把
// question / header / options / multiSelect 回填到结构里。
TEST(AskUserQuestionValidateTest, MinimalValidInputIsAccepted) {
    std::string err;
    auto out = validate_ask_user_question_args(
        R"({
            "questions": [{
                "question": "Which library?",
                "header": "Library",
                "options": [
                    {"label": "axios", "description": "HTTP with promises"},
                    {"label": "fetch", "description": "Native browser API"}
                ]
            }]
        })", err);
    ASSERT_TRUE(out.has_value()) << err;
    EXPECT_TRUE(err.empty());
    ASSERT_EQ(out->size(), 1u);
    EXPECT_EQ((*out)[0].question, "Which library?");
    EXPECT_EQ((*out)[0].header, "Library");
    EXPECT_FALSE((*out)[0].multi_select);
    ASSERT_EQ((*out)[0].options.size(), 2u);
    EXPECT_EQ((*out)[0].options[0].label, "axios");
}

// 场景:questions 长度越界(0 题或 5 题)应被拒,错误信息里包含 "questions"。
TEST(AskUserQuestionValidateTest, QuestionsLengthOutOfRangeRejected) {
    std::string err;
    auto empty = validate_ask_user_question_args(
        R"({"questions": []})", err);
    EXPECT_FALSE(empty.has_value());
    EXPECT_NE(err.find("questions"), std::string::npos) << err;

    err.clear();
    auto too_many = validate_ask_user_question_args(
        R"({"questions": [
            {"question":"A?", "header":"A",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]},
            {"question":"B?", "header":"B",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]},
            {"question":"C?", "header":"C",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]},
            {"question":"D?", "header":"D",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]},
            {"question":"E?", "header":"E",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]}
        ]})", err);
    EXPECT_FALSE(too_many.has_value());
    EXPECT_NE(err.find("questions"), std::string::npos) << err;
}

// 场景:某题 options 长度越界(1 或 5)应被拒,错误信息里包含 "options"。
TEST(AskUserQuestionValidateTest, OptionsLengthOutOfRangeRejected) {
    std::string err;
    auto too_few = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?","header":"H",
            "options":[{"label":"only","description":""}]
        }]})", err);
    EXPECT_FALSE(too_few.has_value());
    EXPECT_NE(err.find("options"), std::string::npos) << err;

    err.clear();
    auto too_many = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?","header":"H",
            "options":[
                {"label":"1","description":""},
                {"label":"2","description":""},
                {"label":"3","description":""},
                {"label":"4","description":""},
                {"label":"5","description":""}
            ]
        }]})", err);
    EXPECT_FALSE(too_many.has_value());
    EXPECT_NE(err.find("options"), std::string::npos) << err;
}

// 场景:两题 question 文本完全相同 → 被拒,错误信息里包含 "unique"。
TEST(AskUserQuestionValidateTest, DuplicateQuestionTextsRejected) {
    std::string err;
    auto out = validate_ask_user_question_args(
        R"({"questions":[
            {"question":"Same?","header":"A",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]},
            {"question":"Same?","header":"B",
             "options":[{"label":"1","description":""},{"label":"2","description":""}]}
        ]})", err);
    EXPECT_FALSE(out.has_value());
    EXPECT_NE(err.find("unique"), std::string::npos) << err;
}

// 场景:同一题里两个 option 的 label 完全相同 → 被拒,错误信息里
// 包含 "labels must be unique"(子串匹配)。
TEST(AskUserQuestionValidateTest, DuplicateOptionLabelsRejected) {
    std::string err;
    auto out = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?","header":"H",
            "options":[
                {"label":"same","description":"first"},
                {"label":"same","description":"second"}
            ]
        }]})", err);
    EXPECT_FALSE(out.has_value());
    EXPECT_NE(err.find("labels must be unique"), std::string::npos) << err;
}

// 场景:header 字符数 13(这里用 13 个中文字符,UTF-8 是 39 字节)→ 被拒,
// 错误信息包含 "header"。同时验证 header 字符数 12 的中文串是合法的
// (边界验证),避免把按字节数判断的实现误放过。
TEST(AskUserQuestionValidateTest, HeaderTooLongByCharCountRejected) {
    std::string err;
    auto too_long = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?",
            "header":"一二三四五六七八九十十一十二十三",
            "options":[{"label":"1","description":""},{"label":"2","description":""}]
        }]})", err);
    EXPECT_FALSE(too_long.has_value());
    EXPECT_NE(err.find("header"), std::string::npos) << err;

    err.clear();
    // 边界 12 字符:10 个 CJK + 2 个 ASCII,共 12 codepoints。这里故意混合 CJK
    // 与 ASCII,避免因代码 bug 按字节算时,全 CJK 字符串误被放过(30 bytes)。
    auto boundary = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?",
            "header":"一二三四五六七八九十AB",
            "options":[{"label":"1","description":""},{"label":"2","description":""}]
        }]})", err);
    EXPECT_TRUE(boundary.has_value()) << err;
}

// 场景:option 里出现 preview 字符串字段 → 校验通过,preview 不进入
// 返回结构。(AskOption 本身不持有 preview —— 校验层吞掉即可。)
TEST(AskUserQuestionValidateTest, PreviewFieldIsAcceptedButIgnored) {
    std::string err;
    auto out = validate_ask_user_question_args(
        R"({"questions":[{
            "question":"Q?","header":"H",
            "options":[
                {"label":"a","description":"d","preview":"<pre>ignored</pre>"},
                {"label":"b","description":"d"}
            ]
        }]})", err);
    ASSERT_TRUE(out.has_value()) << err;
    // AskOption 结构上没有 preview 字段 —— 编译即证明了"ignore";
    // 这里额外确认返回值里两个 option 都齐整。
    ASSERT_EQ((*out)[0].options.size(), 2u);
}

// 场景:format_ask_answers 单题单答,拼接与上游一致。
TEST(AskUserQuestionFormatTest, SingleQuestionSingleAnswer) {
    std::vector<std::string> order{"Which library?"};
    std::map<std::string, std::string> ans{{"Which library?", "axios"}};
    EXPECT_EQ(format_ask_answers(order, ans),
              "User has answered your questions: \"Which library?\"=\"axios\"");
}

// 场景:成功问答的 UI metadata 保留原始问题顺序和最终答案文本,供
// desktop/web 渲染确认卡片,但不参与 provider-visible output 拼接。
TEST(AskUserQuestionFormatTest, StructuredResultMetadataKeepsOrderedPairs) {
    std::vector<std::string> order{"Q1?", "Q2?"};
    std::map<std::string, std::string> ans{
        {"Q1?", "直接修改并补测试"},
        {"Q2?", "onBeforeUnmount"}
    };

    auto meta = build_ask_user_question_result_metadata(order, ans);
    ASSERT_TRUE(meta.contains("ask_user_question_result"));
    const auto& result = meta["ask_user_question_result"];
    ASSERT_TRUE(result["items"].is_array());
    ASSERT_EQ(result["items"].size(), 2u);
    EXPECT_EQ(result["items"][0]["question"], "Q1?");
    EXPECT_EQ(result["items"][0]["answer"], "直接修改并补测试");
    EXPECT_EQ(result["items"][1]["question"], "Q2?");
    EXPECT_EQ(result["items"][1]["answer"], "onBeforeUnmount");
}

// 场景:TUI 等文本界面可以从同一份 UI metadata 生成紧凑 Q/A 留档,
// 而不是显示 provider-visible 的英文 tool output。
TEST(AskUserQuestionFormatTest, StructuredResultMetadataFormatsDisplayText) {
    std::vector<std::string> order{"Q1?", "Q2?"};
    std::map<std::string, std::string> ans{
        {"Q1?", "直接修改并补测试"},
        {"Q2?", "onBeforeUnmount"}
    };

    auto meta = build_ask_user_question_result_metadata(order, ans);

    EXPECT_EQ(format_ask_user_question_result_display(meta),
              "已确认 2 项\n"
              "Q  Q1?\n"
              "A  直接修改并补测试\n"
              "---\n"
              "Q  Q2?\n"
              "A  onBeforeUnmount");
}

// 场景:缺失或畸形 metadata 不应污染 UI,调用方据此回退旧输出。
TEST(AskUserQuestionFormatTest, MalformedResultMetadataHasNoDisplayText) {
    EXPECT_TRUE(format_ask_user_question_result_display(nlohmann::json::object()).empty());
    EXPECT_TRUE(format_ask_user_question_result_display({
        {"ask_user_question_result", {{"items", nlohmann::json::array({42})}}}
    }).empty());
}

// 场景:两题、第二题为 multi-select(调用方已经把多个 label 用 ", "
// 拼成单字符串),format 保持顺序 + 分隔符。
TEST(AskUserQuestionFormatTest, MultiQuestionWithMultiSelect) {
    std::vector<std::string> order{"Q1?", "Q2?"};
    std::map<std::string, std::string> ans{
        {"Q1?", "axios"},
        {"Q2?", "TypeScript, Prettier"}
    };
    EXPECT_EQ(format_ask_answers(order, ans),
              "User has answered your questions: \"Q1?\"=\"axios\", "
              "\"Q2?\"=\"TypeScript, Prettier\"");
}

// 场景:答案里含 `"` —— format 不做转义(和 claudecodehaha 同行为,
// 作为已记录的已知现象)。此 TEST 把未转义的 `"` 硬写进期望字符串里。
TEST(AskUserQuestionFormatTest, QuoteInAnswerIsNotEscaped) {
    std::vector<std::string> order{"Quote?"};
    std::map<std::string, std::string> ans{{"Quote?", "He said \"hi\""}};
    std::string out = format_ask_answers(order, ans);
    EXPECT_EQ(out,
              "User has answered your questions: \"Quote?\"=\"He said \"hi\"\"");
    // 额外断言:原样出现 3 对以上未转义双引号(Q/A 各一对 + 答案内 2 个 = 6)。
    EXPECT_GE(std::count(out.begin(), out.end(), '"'), 6);
}

// 场景:拒绝路径固定 ToolResult —— success=false 且 output 精确匹配。
TEST(AskUserQuestionRejectedTest, ConstantRejectedResult) {
    auto r = make_rejected_ask_result();
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.output, "[Error] User declined to answer questions.");
}

// —— AskUserQuestion 双入口答案串拼装(format_single_answer)——
// 场景:仅预设 —— 现状的多选 ", " 拼合行为保持不变。
TEST(AskUserQuestionDualEntryFormatTest, PresetsOnlyJoinsWithCommaSpace) {
    EXPECT_EQ(format_single_answer({"A", "B"}, "", ""), "A, B");
    EXPECT_EQ(format_single_answer({"Docker 容器部署", "K8s Helm Chart"}, "", ""),
              "Docker 容器部署, K8s Helm Chart");
    EXPECT_EQ(format_single_answer({}, "", ""), "");
    // 空 label 被跳过(防御,正常输入不会出现)。
    EXPECT_EQ(format_single_answer({"A", "", "B"}, "", ""), "A, B");
}

// 场景:预设 + 补充 —— 补充文本以 "; 补充: " 追加在已选预设之后。
TEST(AskUserQuestionDualEntryFormatTest, PresetsPlusSupplementAppendsMarker) {
    EXPECT_EQ(format_single_answer({"A"}, "预算上限 5000", ""),
              "A; 补充: 预算上限 5000");
    EXPECT_EQ(format_single_answer({"A", "B"}, "补充说明", ""),
              "A, B; 补充: 补充说明");
}

// 场景:仅补充(独立存在,不选任何预设)—— 决策 3 允许,产物只有补充标记。
TEST(AskUserQuestionDualEntryFormatTest, SupplementAloneIsIndependent) {
    EXPECT_EQ(format_single_answer({}, "自定义文本", ""), "补充: 自定义文本");
}

// 场景:以上都不是(独占)—— 预设已清空,产物只有独占标记;即使 selected
// 或 supplement 残留(外部直连违规的防御输入),独占仍然优先(决策 2 互斥 +
// 协议兜底:以 exclusive_text 为准)。
TEST(AskUserQuestionDualEntryFormatTest, ExclusiveVoidsPresetsAndSupplement) {
    EXPECT_EQ(format_single_answer({}, "", "都不合适，我们用裸机自托管"),
              "以上都不是: 都不合适，我们用裸机自托管");
    // 防御:selected 残留也被忽略。
    EXPECT_EQ(format_single_answer({"A", "B"}, "", "都不合适"),
              "以上都不是: 都不合适");
    // 防御:exclusive 与 supplement 同时非空 → exclusive 为准。
    EXPECT_EQ(format_single_answer({"A"}, "不该出现的补充", "都不合适"),
              "以上都不是: 都不合适");
}

// 场景:标记词与分隔符均为单点常量(grill Q3:产物标记固定中文、分隔符
// ASCII)——精确断言整个产物,防止未来改动静默破坏模型侧格式契约。
TEST(AskUserQuestionDualEntryFormatTest, ExactMarkersAndSeparators) {
    EXPECT_EQ(format_single_answer({"A", "B"}, "x", ""),
              "A, B; 补充: x");
    EXPECT_EQ(format_single_answer({}, "", "y"),
              "以上都不是: y");
    // supplement 单独时无多余前缀。
    EXPECT_EQ(format_single_answer({}, "x", ""), "补充: x");
}


// active goal 仍使用提问组件，但固定 30 秒超时，到期自动采纳
// 每题第一个(推荐)选项。callback 在这里模拟 prompter 到期。
TEST(AskUserQuestionGoalTest, AsyncToolPromptsThenAdoptsRecommendedAfterThirtySeconds) {
    auto tool = acecode::create_ask_user_question_tool_async();
    acecode::ToolContext ctx;
    bool prompter_called = false;
    ctx.ask_user_questions = [&](const nlohmann::json&) {
        prompter_called = true;
        return nlohmann::json{{"cancelled", false}, {"timed_out", true}};
    };
    ctx.goal_unattended_active = [] { return true; };

    const std::string args = R"({"questions":[{"question":"Pick one?",
        "header":"choice","options":[{"label":"A","description":"a"},
        {"label":"B","description":"b"}]}]})";
    auto r = tool.execute(args, ctx);
    EXPECT_TRUE(r.success) << r.output;
    EXPECT_TRUE(prompter_called);
    EXPECT_NE(r.output.find("30 seconds"), std::string::npos);
    EXPECT_NE(r.output.find("\"Pick one?\"=\"A\""), std::string::npos);
    ASSERT_TRUE(r.metadata.contains("ask_user_question_auto"));
    EXPECT_EQ(r.metadata["ask_user_question_auto"].value("mode", ""), "timeout");
    EXPECT_EQ(r.metadata["ask_user_question_auto"].value("seconds", 0), 30);
}

// 场景:非 goal 模式(探针缺省 / 返回 false)行为不变 —— 仍走 prompter。
// 这里 prompter 返回 cancelled=true,期望拿到既有的拒绝结果。
TEST(AskUserQuestionUnattendedTest, AsyncToolStillPromptsWithoutActiveGoal) {
    auto tool = acecode::create_ask_user_question_tool_async();
    acecode::ToolContext ctx;
    bool prompter_called = false;
    ctx.ask_user_questions = [&](const nlohmann::json&) {
        prompter_called = true;
        return nlohmann::json{{"cancelled", true}};
    };
    ctx.goal_unattended_active = [] { return false; };

    const std::string args = R"({"questions":[{"question":"Pick one?",
        "header":"choice","options":[{"label":"A","description":"a"},
        {"label":"B","description":"b"}]}]})";
    auto r = tool.execute(args, ctx);
    EXPECT_TRUE(prompter_called);
    EXPECT_FALSE(r.success);
}

// ── TUI 传输层 ──────────────────────────────────────
//
// 工具逻辑与 TUI 传输已拆开:两端共用 create_ask_user_question_tool_async(),
// TUI 只提供 ask_via_tui_overlay 这个 `json(json)` 通道(由 AgentLoop 注入到
// ToolContext::ask_user_questions)。超时时长与来源标注现在由 AgentLoop 算好
// 传进来 —— 与 daemon 给 prompter 算 timeout_override 是同一处职责。
// 因此这里直接驱动通道,而不再经工具。

namespace {

nlohmann::json single_question_payload() {
    return nlohmann::json::array({
        nlohmann::json{
            {"id", "Pick one?"},
            {"text", "Pick one?"},
            {"header", "choice"},
            {"multiSelect", false},
            {"options", nlohmann::json::array({
                nlohmann::json{{"label", "A"}, {"value", "A"}, {"description", "recommended"}},
                nlohmann::json{{"label", "B"}, {"value", "B"}, {"description", "alternative"}},
            })},
        },
    });
}

} // namespace

TEST(TuiAskChannelTest, TimeoutReportsTimedOutAndCleansOverlay) {
    // 触发场景:question_policy=timeout 且无人回答。
    // 期望行为:到期返回 timed_out=true 并把 overlay 状态清干净
    // (残留的 ask_pending / ask_questions 会让下一次提问渲染脏数据)。
    // 注意:采纳推荐项本身不在这一层 —— 那是工具层
    // make_timeout_adopted_ask_result 的职责,与 daemon 路径共用一份。
    acecode::TuiState state;
    auto screen = ftxui::ScreenInteractive::FitComponent();
    std::atomic<bool> abort{false};

    const auto started = std::chrono::steady_clock::now();
    const nlohmann::json response = acecode::tui::ask_via_tui_overlay(
        state, screen, single_question_payload(), &abort,
        /*timeout_seconds=*/1, /*origin_label=*/"");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_TRUE(response.value("timed_out", false));
    EXPECT_FALSE(response.value("cancelled", true));
    EXPECT_GE(elapsed, std::chrono::milliseconds(900));

    std::lock_guard<std::mutex> lk(state.mu);
    EXPECT_FALSE(state.ask_pending);
    EXPECT_TRUE(state.ask_questions.empty());
    EXPECT_EQ(state.ask_timeout_hint_seconds, 0);
}

TEST(TuiAskChannelTest, TimeoutHintIsShownAndEarlyAnswerWins) {
    // 触发场景:active goal 的 30 秒窗口(由 AgentLoop 算好传进来)。
    // 期望行为:overlay 顶部显示 30 秒提示;用户在到期前回答时采用真实
    // 答案而不是超时采纳 —— 用户真实意志优先。
    acecode::TuiState state;
    auto screen = ftxui::ScreenInteractive::FitComponent();
    std::atomic<bool> abort{false};

    auto future = std::async(std::launch::async, [&] {
        return acecode::tui::ask_via_tui_overlay(
            state, screen, single_question_payload(), &abort,
            /*timeout_seconds=*/30, /*origin_label=*/"");
    });
    if (!wait_for_ask_overlay(state, std::chrono::seconds(2))) {
        abort.store(true);
        state.ask_cv.notify_all();
        (void)future.get();
        FAIL() << "goal AskUserQuestion overlay did not open";
    }

    {
        std::lock_guard<std::mutex> lk(state.mu);
        EXPECT_EQ(state.ask_timeout_hint_seconds, 30);
        // 双入口改造后通道从结构化字段组装答案,不再读 ask_result_answers:
        // 模拟用户在 overlay 里 Enter 提交下标 1 的选项(label "B")并确认。
        state.ask_selected_options[0] = 1;
        state.ask_result_ok = true;
        state.ask_pending = false;
    }
    state.ask_cv.notify_all();

    const nlohmann::json response = future.get();
    EXPECT_FALSE(response.value("timed_out", true));
    EXPECT_FALSE(response.value("cancelled", true));
    ASSERT_TRUE(response.contains("answers"));
    ASSERT_EQ(response["answers"].size(), 1u);
    EXPECT_EQ(response["answers"][0]["question_id"], "Pick one?");
    EXPECT_EQ(response["answers"][0]["selected"][0], "B");

    std::lock_guard<std::mutex> lk(state.mu);
    EXPECT_EQ(state.ask_timeout_hint_seconds, 0);
}

TEST(TuiAskChannelTest, OriginLabelMarksSubagentQuestions) {
    // 子代理提问时 overlay 要标出来源,否则用户不知道是谁在问。
    // 这一位现在由 AgentLoop 从 session_manager 算好传进来。
    acecode::TuiState state;
    auto screen = ftxui::ScreenInteractive::FitComponent();
    std::atomic<bool> abort{false};

    auto future = std::async(std::launch::async, [&] {
        return acecode::tui::ask_via_tui_overlay(
            state, screen, single_question_payload(), &abort,
            /*timeout_seconds=*/0, /*origin_label=*/"[subagent] child task");
    });
    ASSERT_TRUE(wait_for_ask_overlay(state, std::chrono::seconds(2)));

    {
        std::lock_guard<std::mutex> lk(state.mu);
        EXPECT_EQ(state.ask_origin_label, "[subagent] child task");
        state.ask_result_ok = false;
        state.ask_pending = false;
    }
    state.ask_cv.notify_all();
    (void)future.get();

    std::lock_guard<std::mutex> lk(state.mu);
    EXPECT_TRUE(state.ask_origin_label.empty());
}

TEST(TuiAskChannelTest, AbortBeforeOpeningReturnsCancelled) {
    // 已经在中止中时不去动 TUI。
    acecode::TuiState state;
    auto screen = ftxui::ScreenInteractive::FitComponent();
    std::atomic<bool> abort{true};

    const nlohmann::json response = acecode::tui::ask_via_tui_overlay(
        state, screen, single_question_payload(), &abort, 0, "");

    EXPECT_TRUE(response.value("cancelled", false));
    std::lock_guard<std::mutex> lk(state.mu);
    EXPECT_FALSE(state.ask_pending);
}

// 双入口(ask-user-question-dual-entry):「我要补充」与预设共存进入产物。
TEST(TuiAskChannelTest, DualEntrySupplementAppendsToPresets) {
    acecode::TuiState state;
    auto screen = ftxui::ScreenInteractive::FitComponent();
    std::atomic<bool> abort{false};

    auto future = std::async(std::launch::async, [&] {
        return acecode::tui::ask_via_tui_overlay(
            state, screen, single_question_payload(), &abort,
            /*timeout_seconds=*/0, /*origin_label=*/"");
    });
    ASSERT_TRUE(wait_for_ask_overlay(state, std::chrono::seconds(2)));

    {
        std::lock_guard<std::mutex> lk(state.mu);
        state.ask_selected_options[0] = 1; // 单选下标 1 = label "B"
        state.ask_supplement_text[0] = "note the caveat";
        state.ask_result_ok = true;
        state.ask_pending = false;
    }
    state.ask_cv.notify_all();

    const nlohmann::json response = future.get();
    EXPECT_FALSE(response.value("cancelled", true));
    ASSERT_TRUE(response.contains("answers"));
    ASSERT_EQ(response["answers"].size(), 1u);
    EXPECT_EQ(response["answers"][0]["question_id"], "Pick one?");
    ASSERT_EQ(response["answers"][0]["selected"].size(), 1u);
    EXPECT_EQ(response["answers"][0]["selected"][0], "B");
    EXPECT_EQ(response["answers"][0]["supplement_text"], "note the caveat");
    EXPECT_FALSE(response["answers"][0].contains("exclusive_text"));
}

// 双入口(ask-user-question-dual-entry):「以上都不是」激活 → 预设作废、
// 补充旧文本被 active-filter 压制,只发 exclusive_text。
TEST(TuiAskChannelTest, DualEntryExclusiveVoidsPresetsAndSupplement) {
    acecode::TuiState state;
    auto screen = ftxui::ScreenInteractive::FitComponent();
    std::atomic<bool> abort{false};

    auto future = std::async(std::launch::async, [&] {
        return acecode::tui::ask_via_tui_overlay(
            state, screen, single_question_payload(), &abort,
            /*timeout_seconds=*/0, /*origin_label=*/"");
    });
    ASSERT_TRUE(wait_for_ask_overlay(state, std::chrono::seconds(2)));

    {
        std::lock_guard<std::mutex> lk(state.mu);
        state.ask_exclusive_active[0] = true;
        state.ask_exclusive_text[0] = "our own answer";
        state.ask_supplement_text[0] = "stale supplement";
        state.ask_selected_options[0] = 1; // 防御:也应被独占作废
        state.ask_result_ok = true;
        state.ask_pending = false;
    }
    state.ask_cv.notify_all();

    const nlohmann::json response = future.get();
    EXPECT_FALSE(response.value("cancelled", true));
    ASSERT_TRUE(response.contains("answers"));
    ASSERT_EQ(response["answers"].size(), 1u);
    EXPECT_TRUE(response["answers"][0]["selected"].empty()); // 预设被作废
    EXPECT_EQ(response["answers"][0]["exclusive_text"], "our own answer");
    EXPECT_FALSE(response["answers"][0].contains("supplement_text"));
}
