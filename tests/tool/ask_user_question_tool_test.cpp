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
#include <nlohmann/json.hpp>

#include "tool/ask_user_question_tool.hpp"
#include "tui/tui_ask_channel.hpp"
#include "tui/tui_state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using acecode::AskQuestion;
using acecode::AskOption;
using acecode::build_ask_user_question_result_metadata;
using acecode::format_ask_answers;
using acecode::format_ask_user_question_result_display;
using acecode::make_rejected_ask_result;
using acecode::validate_ask_user_question_args;

namespace {

constexpr const char* kInteractiveQuestionArgs = R"({
    "questions": [{
        "question": "Pick one?",
        "header": "choice",
        "options": [
            {"label": "A [Recommended]", "description": "recommended"},
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

std::string questions_json(std::size_t count) {
    nlohmann::json questions = nlohmann::json::array();
    for (std::size_t i = 0; i < count; ++i) {
        questions.push_back({
            {"question", "Question " + std::to_string(i) + "?"},
            {"header", "Q" + std::to_string(i)},
            {"options", nlohmann::json::array({
                nlohmann::json{{"label", "A"}, {"description", "first"}},
                nlohmann::json{{"label", "B"}, {"description", "second"}},
            })},
        });
    }
    return nlohmann::json{{"questions", std::move(questions)}}.dump();
}

// 单题、可指定选项数量;label/description 用必填字段填充。
std::string single_question_with_options(std::size_t option_count) {
    nlohmann::json options = nlohmann::json::array();
    for (std::size_t i = 0; i < option_count; ++i) {
        options.push_back({
            {"label", "Option " + std::to_string(i)},
            {"description", "description " + std::to_string(i)},
        });
    }
    return nlohmann::json{{"questions", nlohmann::json::array({
        {{"question", "Which option?"},
         {"header", "Pick"},
         {"options", std::move(options)}}
    })}}.dump();
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

// 场景:默认上限允许 10 题,超过默认上限的 11 题被拒绝。
TEST(AskUserQuestionValidateTest, DefaultQuestionLimitIsTen) {
    std::string err;
    auto ten = validate_ask_user_question_args(questions_json(10), err);
    ASSERT_TRUE(ten.has_value()) << err;
    ASSERT_EQ(ten->size(), 10u);

    err.clear();
    auto eleven = validate_ask_user_question_args(questions_json(11), err);
    EXPECT_FALSE(eleven.has_value());
    EXPECT_NE(err.find("between 1 and 10"), std::string::npos) << err;
    EXPECT_NE(err.find("got 11"), std::string::npos) << err;
    EXPECT_NE(err.find("Split the questions"), std::string::npos) << err;
}

TEST(AskUserQuestionValidateTest, CustomQuestionLimitIsApplied) {
    std::string err;
    auto three = validate_ask_user_question_args(questions_json(3), err, 3);
    ASSERT_TRUE(three.has_value()) << err;

    err.clear();
    auto four = validate_ask_user_question_args(questions_json(4), err, 3);
    EXPECT_FALSE(four.has_value());
    EXPECT_NE(err.find("between 1 and 3"), std::string::npos) << err;
    EXPECT_NE(err.find("got 4"), std::string::npos) << err;
    EXPECT_NE(err.find("Split the questions"), std::string::npos) << err;
}

TEST(AskUserQuestionValidateTest, CustomLimitIsDefensivelyClamped) {
    std::string err;
    auto one = validate_ask_user_question_args(questions_json(1), err, 0);
    EXPECT_TRUE(one.has_value()) << err;

    err.clear();
    auto two = validate_ask_user_question_args(questions_json(2), err, 51);
    EXPECT_TRUE(two.has_value()) << err;
}

// 场景:默认选项上限为 6 —— 6 个选项通过,7 个被拒,错误文案带当前上限。
TEST(AskUserQuestionValidateTest, DefaultOptionLimitIsSix) {
    std::string err;
    auto six = validate_ask_user_question_args(single_question_with_options(6), err);
    ASSERT_TRUE(six.has_value()) << err;
    ASSERT_EQ((*six)[0].options.size(), 6u);

    err.clear();
    auto seven = validate_ask_user_question_args(single_question_with_options(7), err);
    EXPECT_FALSE(seven.has_value());
    EXPECT_NE(err.find("between 2 and 6"), std::string::npos) << err;
    EXPECT_NE(err.find("got 7"), std::string::npos) << err;
}

// 场景:自定义选项上限 8 时,8 个通过、9 个被拒,错误文案带当前上限。
TEST(AskUserQuestionValidateTest, CustomOptionLimitIsApplied) {
    std::string err;
    auto eight = validate_ask_user_question_args(
        single_question_with_options(8), err,
        acecode::kDefaultAskMaxQuestions, 8);
    ASSERT_TRUE(eight.has_value()) << err;
    ASSERT_EQ((*eight)[0].options.size(), 8u);

    err.clear();
    auto nine = validate_ask_user_question_args(
        single_question_with_options(9), err,
        acecode::kDefaultAskMaxQuestions, 8);
    EXPECT_FALSE(nine.has_value());
    EXPECT_NE(err.find("between 2 and 8"), std::string::npos) << err;
    EXPECT_NE(err.find("got 9"), std::string::npos) << err;
}

// 场景:选项上限被防御性钳制到 [4,8] —— 9 当 8 用,3 当 4 用。
TEST(AskUserQuestionValidateTest, OptionLimitIsDefensivelyClamped) {
    std::string err;
    auto eight = validate_ask_user_question_args(
        single_question_with_options(8), err,
        acecode::kDefaultAskMaxQuestions, 9);
    EXPECT_TRUE(eight.has_value()) << err;

    err.clear();
    auto nine = validate_ask_user_question_args(
        single_question_with_options(9), err,
        acecode::kDefaultAskMaxQuestions, 9);
    EXPECT_FALSE(nine.has_value());

    err.clear();
    auto four = validate_ask_user_question_args(
        single_question_with_options(4), err,
        acecode::kDefaultAskMaxQuestions, 3);
    EXPECT_TRUE(four.has_value()) << err;

    err.clear();
    auto five = validate_ask_user_question_args(
        single_question_with_options(5), err,
        acecode::kDefaultAskMaxQuestions, 3);
    EXPECT_FALSE(five.has_value());
}

// 场景:下限固定为 2,不受配置影响。
TEST(AskUserQuestionValidateTest, OptionFloorStaysAtTwo) {
    std::string err;
    auto two = validate_ask_user_question_args(single_question_with_options(2), err);
    ASSERT_TRUE(two.has_value()) << err;

    err.clear();
    auto one = validate_ask_user_question_args(single_question_with_options(1), err);
    EXPECT_FALSE(one.has_value());
    EXPECT_NE(err.find("between 2 and"), std::string::npos) << err;
}

TEST(AskUserQuestionSchemaTest, SchemaUsesConfiguredQuestionLimit) {
    const auto default_tool = acecode::create_ask_user_question_tool_async();
    EXPECT_EQ(default_tool.definition.parameters["properties"]["questions"]["maxItems"], 10);

    const auto custom_tool = acecode::create_ask_user_question_tool_async(3);
    EXPECT_EQ(custom_tool.definition.parameters["properties"]["questions"]["maxItems"], 3);
    EXPECT_NE(custom_tool.definition.parameters["properties"]["questions"]["description"]
                  .get<std::string>().find("1-3 questions"),
              std::string::npos);
}

TEST(AskUserQuestionSchemaTest, SchemaFollowsConfiguredOptionLimit) {
    const auto default_tool = acecode::create_ask_user_question_tool_async();
    const auto& default_options =
        default_tool.definition.parameters["properties"]["questions"]["items"]["properties"]["options"];
    EXPECT_EQ(default_options["minItems"], 2);
    EXPECT_EQ(default_options["maxItems"], 6);
    EXPECT_NE(default_options["description"].get<std::string>().find("2-6 mutually"),
              std::string::npos);

    const auto custom_tool = acecode::create_ask_user_question_tool_async(10, 8);
    const auto& custom_options =
        custom_tool.definition.parameters["properties"]["questions"]["items"]["properties"]["options"];
    EXPECT_EQ(custom_options["maxItems"], 8);
    EXPECT_NE(custom_options["description"].get<std::string>().find("2-8 mutually"),
              std::string::npos);

    const auto clamped_tool = acecode::create_ask_user_question_tool_async(10, 9);
    const auto& clamped_options =
        clamped_tool.definition.parameters["properties"]["questions"]["items"]["properties"]["options"];
    EXPECT_EQ(clamped_options["maxItems"], 8);
}

TEST(AskUserQuestionExecutionTest, ConfiguredLimitRejectsBeforeOpeningChannel) {
    const auto tool = acecode::create_ask_user_question_tool_async(3);
    acecode::ToolContext ctx;
    bool channel_called = false;
    ctx.ask_user_questions = [&](const nlohmann::json&) {
        channel_called = true;
        return nlohmann::json{{"cancelled", false}};
    };

    const auto result = tool.execute(questions_json(4), ctx);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(channel_called);
    EXPECT_NE(result.output.find("between 1 and 3"), std::string::npos);
    EXPECT_NE(result.output.find("got 4"), std::string::npos);
    EXPECT_NE(result.output.find("Split the questions"), std::string::npos);
}

TEST(AskUserQuestionExecutionTest, ConfiguredOptionLimitRejectsBeforeOpeningChannel) {
    const auto tool = acecode::create_ask_user_question_tool_async(10, 6);
    acecode::ToolContext ctx;
    bool channel_called = false;
    ctx.ask_user_questions = [&](const nlohmann::json&) {
        channel_called = true;
        return nlohmann::json{{"cancelled", false}};
    };

    const auto result = tool.execute(single_question_with_options(7), ctx);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(channel_called);
    EXPECT_NE(result.output.find("between 2 and 6"), std::string::npos);
    EXPECT_NE(result.output.find("got 7"), std::string::npos);
}

// 场景:某题 options 长度越界(1 或 7)应被拒,错误信息里包含 "options"。
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
                {"label":"5","description":""},
                {"label":"6","description":""},
                {"label":"7","description":""}
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
    EXPECT_FALSE(result["items"][0]["auto_selected"]);
    EXPECT_FALSE(result["items"][1]["auto_selected"]);
}

// 场景:TUI 等文本界面可以从同一份 UI metadata 生成紧凑 Q/A 留档,
// 而不是显示 provider-visible 的英文 tool output。每题一项、问题与答案成对,
// 项间空一行,问题永远不能被省略。
TEST(AskUserQuestionFormatTest, StructuredResultMetadataFormatsDisplayText) {
    std::vector<std::string> order{"Q1?", "Q2?"};
    std::map<std::string, std::string> ans{
        {"Q1?", "直接修改并补测试"},
        {"Q2?", "onBeforeUnmount"}
    };

    auto meta = build_ask_user_question_result_metadata(order, ans);

    EXPECT_EQ(format_ask_user_question_result_display(meta),
              "1. Q1?\xEF\xBC\x9A直接修改并补测试\n"
              "\n"
              "2. Q2?\xEF\xBC\x9AonBeforeUnmount");
}

TEST(AskUserQuestionFormatTest, StructuredResultMetadataFormatsAutoSelectedDisplayText) {
    std::vector<std::string> order{"Q1?", "Q2?"};
    std::map<std::string, std::string> ans{
        {"Q1?", "Recommended"},
        {"Q2?", "Not answered"}
    };
    std::set<std::string> auto_selected{"Q1?"};

    const auto meta = build_ask_user_question_result_metadata(
        order, ans, &auto_selected);

    EXPECT_EQ(format_ask_user_question_result_display(meta),
              "1. Q1?\xEF\xBC\x9A[Auto-selected] Recommended\n"
              "\n"
              "2. Q2?\xEF\xBC\x9ANot answered");
}

// 场景:带 [Recommended] 后缀的 label 与超时自动选择标记共存时,
// 问题文本不能被答案吞掉或替换。
TEST(AskUserQuestionFormatTest, DisplayTextNeverDropsQuestionText) {
    std::vector<std::string> order{"希望我直接修改还是先给出方案?"};
    std::map<std::string, std::string> ans{
        {"希望我直接修改还是先给出方案?", "先给方案"}
    };
    const auto meta = build_ask_user_question_result_metadata(order, ans);
    const std::string display = format_ask_user_question_result_display(meta);
    EXPECT_NE(display.find("希望我直接修改还是先给出方案?"), std::string::npos);
    EXPECT_NE(display.find("先给方案"), std::string::npos);
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
// metadata 必须带 cancelled 标记:「已取消全部回答」反馈卡靠它作为落盘依据,
// 才能在回合结束/会话重载后持续展示。
TEST(AskUserQuestionRejectedTest, ConstantRejectedResult) {
    auto r = make_rejected_ask_result();
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.output, "[Error] User declined to answer questions.");
    ASSERT_TRUE(r.metadata.is_object());
    const auto& result = r.metadata["ask_user_question_result"];
    EXPECT_TRUE(result["cancelled"].get<bool>());
    ASSERT_TRUE(result["items"].is_array());
    EXPECT_TRUE(result["items"].empty());
}

// 场景:多选标记必须随元数据落盘。反馈卡会在已持久化的消息上重建,
// 不落 multi_select 就会在重载后丢掉「(多选)」标注。
TEST(AskUserQuestionFormatTest, StructuredResultMetadataPersistsMultiSelect) {
    std::vector<std::string> order{"Q1?", "Q2?"};
    std::map<std::string, std::string> ans{
        {"Q1?", "A"},
        {"Q2?", "B, C"}
    };
    std::set<std::string> multi_select{"Q2?"};

    auto meta = build_ask_user_question_result_metadata(order, ans, nullptr, &multi_select);
    const auto& items = meta["ask_user_question_result"]["items"];
    ASSERT_EQ(items.size(), 2u);
    EXPECT_FALSE(items[0]["multi_select"].get<bool>());
    EXPECT_TRUE(items[1]["multi_select"].get<bool>());
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
        "header":"choice","options":[{"label":"A [Recommended]","description":"a"},
        {"label":"B","description":"b"}]}]})";
    auto r = tool.execute(args, ctx);
    EXPECT_TRUE(r.success) << r.output;
    EXPECT_TRUE(prompter_called);
    EXPECT_NE(r.output.find("30 seconds"), std::string::npos);
    EXPECT_NE(r.output.find("\"Pick one?\"=\"A [Recommended]\""), std::string::npos);
    ASSERT_TRUE(r.metadata.contains("ask_user_question_auto"));
    EXPECT_EQ(r.metadata["ask_user_question_auto"].value("mode", ""), "timeout");
    EXPECT_EQ(r.metadata["ask_user_question_auto"].value("seconds", 0), 30);
}

TEST(AskUserQuestionGoalTest, TimeoutPreservesStructuredAnswerMarkers) {
    auto tool = acecode::create_ask_user_question_tool_async();
    acecode::ToolContext ctx;
    ctx.ask_user_questions = [&](const nlohmann::json&) {
        return nlohmann::json{
            {"cancelled", false},
            {"timed_out", true},
            {"answers", nlohmann::json::array({
                nlohmann::json{
                    {"question_id", "Answered?"},
                    {"selected", nlohmann::json::array({"A [Recommended]"})},
                    {"custom_text", ""},
                    {"not_answered", false},
                    {"auto_selected", true},
                },
                nlohmann::json{
                    {"question_id", "Skipped?"},
                    {"selected", nlohmann::json::array()},
                    {"custom_text", ""},
                    {"not_answered", true},
                    {"auto_selected", false},
                },
            })},
        };
    };

    const std::string args = R"({"questions":[
        {"question":"Answered?","header":"one",
         "options":[{"label":"A [Recommended]","description":"a"},
                     {"label":"B","description":"b"}]},
        {"question":"Skipped?","header":"two",
         "options":[{"label":"C","description":"c"},
                     {"label":"D","description":"d"}]}
    ]})";
    const auto result = tool.execute(args, ctx);
    ASSERT_TRUE(result.success) << result.output;
    ASSERT_TRUE(result.metadata.contains("ask_user_question_result"));
    const auto& items = result.metadata["ask_user_question_result"]["items"];
    ASSERT_EQ(items.size(), 2u);
    EXPECT_TRUE(items[0]["auto_selected"]);
    EXPECT_EQ(items[1]["answer"], "Not answered");
}


TEST(AskUserQuestionGoalTest, TimeoutAdoptsStructuredAnswersBeforeFallback) {
    auto tool = acecode::create_ask_user_question_tool_async();
    acecode::ToolContext ctx;
    ctx.ask_user_questions = [&](const nlohmann::json&) {
        return nlohmann::json{
            {"cancelled", false},
            {"timed_out", true},
            {"answers", nlohmann::json::array({
                nlohmann::json{
                    {"question_id", "Pick one?"},
                    {"selected", nlohmann::json::array({"B"})},
                    {"custom_text", ""},
                },
            })},
        };
    };
    ctx.goal_unattended_active = [] { return true; };

    const std::string args = R"({"questions":[{"question":"Pick one?",
        "header":"choice","options":[{"label":"A [Recommended]","description":"a"},
        {"label":"B","description":"b"}]}]})";
    const auto result = tool.execute(args, ctx);
    EXPECT_TRUE(result.success);
    EXPECT_NE(result.output.find("\"Pick one?\"=\"B\""), std::string::npos);
    EXPECT_EQ(result.output.find("\"Pick one?\"=\"A\""), std::string::npos);
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
        "header":"choice","options":[{"label":"A [Recommended]","description":"a"},
        {"label":"B","description":"b"}]}]})";
    auto r = tool.execute(args, ctx);
    EXPECT_TRUE(prompter_called);
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.output, "[Error] User declined to answer questions.");
    ASSERT_TRUE(r.metadata.contains("ask_user_question_result"));
    const auto& feedback = r.metadata.at("ask_user_question_result");
    EXPECT_EQ(feedback.at("cancelled"), true);
    EXPECT_EQ(feedback.at("items"), nlohmann::json::array());
}

// 场景:通道返回 cancelled+interjected(用户在提问挂起时直接发了文本,
// AgentLoop::interject_question 构造的响应)。
// 期望:interjected 先于 cancelled 判定 —— 工具 success=true、output 以
// "[User interjected]" 开头并指向「下一条 user 消息」,metadata 标 interjected,
// 绝不能落到 "[Error] User declined" 的拒绝分支(那会让模型以为用户拒答、
// 自作主张继续,正是本功能要修的时序问题)。
TEST(AskUserQuestionInterjectedTest, InterjectedResponseWinsOverCancelled) {
    auto tool = acecode::create_ask_user_question_tool_async();
    acecode::ToolContext ctx;
    ctx.ask_user_questions = [&](const nlohmann::json&) {
        return nlohmann::json{{"cancelled", true}, {"interjected", true}};
    };
    ctx.goal_unattended_active = [] { return false; };

    auto r = tool.execute(kInteractiveQuestionArgs, ctx);
    EXPECT_TRUE(r.success) << r.output;
    EXPECT_EQ(r.output.rfind("[User interjected]", 0), 0u) << r.output;
    // 请求里最后一条真实 user 消息之前可能插着 session-context 块(也是 user
    // 角色),所以文案说的是「next real user message」而不是「下一条 user 消息」。
    EXPECT_NE(r.output.find("next real user message"), std::string::npos) << r.output;
    EXPECT_EQ(r.output.find("User declined"), std::string::npos);
    ASSERT_TRUE(r.metadata.contains("ask_user_question_result"));
    const auto& feedback = r.metadata.at("ask_user_question_result");
    EXPECT_EQ(feedback.at("interjected"), true);
    EXPECT_EQ(feedback.at("items"), nlohmann::json::array());
    EXPECT_FALSE(feedback.contains("cancelled"))
        << "插话不是拒答,前端不能把它渲染成「已取消全部回答」";
    // 插话结果没有问答对可展示,display 文本留空,转录行回退到 output 原文。
    EXPECT_TRUE(format_ask_user_question_result_display(r.metadata).empty());
}

TEST(AskUserQuestionTimeoutTest, NoRecommendedOptionRemainsNotAnswered) {
    const auto question = [] {
        AskQuestion q;
        q.question = "Choose?";
        q.header = "choice";
        q.options = {{"A", "first"}, {"B", "second"}};
        return q;
    }();
    const auto result = acecode::make_timeout_adopted_ask_result(
        {question}, {"Choose?"}, 10, nullptr);
    EXPECT_TRUE(result.success);
    EXPECT_NE(result.output.find("Not answered"), std::string::npos);
    EXPECT_EQ(result.metadata["ask_user_question_result"]["items"][0]["answer"],
              "Not answered");
    EXPECT_TRUE(result.metadata["ask_user_question_result"]["items"][0]["auto_selected"] == false);
}


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
    // AskQuestionSession owns the question state; TuiState only retains the FIFO queue.
    EXPECT_TRUE(state.ask_queue.empty());
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
        acecode::tui::AskQuestionCompletion completion;
        acecode::tui::AskQuestionAnswer answer;
        answer.selected = {"B"};
        completion.answers.push_back(std::move(answer));
        state.ask_completion_override = std::move(completion);
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
        acecode::tui::AskQuestionCompletion completion;
        completion.cancelled = true;
        state.ask_completion_override = std::move(completion);
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

TEST(TuiAskChannelTest, PreservesDistinctWireIdsForIdenticalDisplayText) {
    acecode::TuiState state;
    state.ask_config.selection_feedback_ms = 0;
    auto screen = ftxui::ScreenInteractive::FitComponent();
    std::atomic<bool> abort{false};
    auto payload = single_question_payload();
    payload[0]["id"] = "theme-palette-first";
    payload.push_back(payload[0]);
    payload[1]["id"] = "theme-palette-second";
    auto future = std::async(std::launch::async, [&] {
        return acecode::tui::ask_via_tui_overlay(
            state, screen, payload, &abort, 5, "");
    });
    if (!wait_for_ask_overlay(state, std::chrono::seconds(2))) {
        abort.store(true);
        state.ask_cv.notify_all();
        (void)future.get();
        FAIL() << "Question overlay did not open";
    }
    {
        std::lock_guard<std::mutex> lock(state.mu);
        state.ask_session->dispatch({acecode::tui::AskQuestionEventKind::ChooseNumber, 1});
        state.ask_session->dispatch({acecode::tui::AskQuestionEventKind::ChooseNumber, 2});
        state.ask_session->dispatch({acecode::tui::AskQuestionEventKind::SubmitFocused});
    }
    state.ask_cv.notify_all();
    const auto response = future.get();
    EXPECT_FALSE(response.value("cancelled", true));
    ASSERT_EQ(response["answers"].size(), 2u);
    EXPECT_EQ(response["answers"][0]["question_id"], "theme-palette-first");
    EXPECT_EQ(response["answers"][1]["question_id"], "theme-palette-second");
    EXPECT_EQ(response["answers"][0]["selected"][0], "A");
    EXPECT_EQ(response["answers"][1]["selected"][0], "B");
}
