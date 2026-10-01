// 覆盖 src/host/session_host/memory_summary.{hpp,cpp} 的纯逻辑
// (openspec unify-memory-system 8.3 / 8.5 / 9.2):
// - 提炼输入:只取用户可见消息,按「用户原话 > 助手最终结论 > 助手中间说明 > 工具
//   输出」裁剪,工具输出单条 2 KB
// - 提炼输出严格校验:多余字段、非法取值、超长整份作废
// - 整合计划解析与整体校验:引用依据、名字合法、手写只读、墓碑、长度上限

#include <gtest/gtest.h>

#include "session_host/memory_summary.hpp"

#include <nlohmann/json.hpp>

namespace {

acecode::ChatMessage msg(const std::string& role, const std::string& content) {
    acecode::ChatMessage m;
    m.role = role;
    m.content = content;
    return m;
}

acecode::MemoryEntry entry(const std::string& name, const std::string& source,
                           const std::string& description = "desc") {
    acecode::MemoryEntry e;
    e.name = name;
    e.description = description;
    e.source = source;
    return e;
}

acecode::MemoryPlanContext context_with(std::vector<acecode::MemoryEntry> entries) {
    acecode::MemoryPlanContext context;
    context.entries = std::move(entries);
    context.observation_ids = {"s-0-4#0", "s-0-4#1"};
    return context;
}

std::string validate(const std::string& plan_json, const acecode::MemoryPlanContext& context) {
    const auto plan = acecode::parse_memory_consolidation_plan(plan_json);
    EXPECT_TRUE(plan.ok) << plan.error;
    return acecode::validate_memory_plan(plan.operations, context);
}

} // namespace

// 场景:会话里有真实用户消息、隐藏的目标上下文、压缩摘要、带工具调用的中间说明、
// 工具输出、技能展开结果与系统消息,从第 1 条开始提炼。
// 期望:只收用户可见的部分,隐藏上下文 / 摘要 / 技能展开 / 系统消息不进;
// 带工具调用的助手文字算「中间说明」,不带的算「最终结论」;起点之前的消息不收。
TEST(MemorySummaryTest, CollectsOnlyVisibleMessagesAfterPosition) {
    std::vector<acecode::ChatMessage> messages;
    messages.push_back(msg("user", "before the extraction point"));
    messages.push_back(msg("user", "please always use pnpm"));
    auto hidden = msg("user", "hidden goal context");
    hidden.metadata = {{"hidden_goal_context", true}};
    messages.push_back(hidden);
    auto summary = msg("user", "compact summary");
    summary.is_compact_summary = true;
    messages.push_back(summary);
    auto working = msg("assistant", "let me check the lockfile");
    working.tool_calls = nlohmann::json::array({{{"id", "1"}}});
    messages.push_back(working);
    messages.push_back(msg("tool", "pnpm-lock.yaml found"));
    messages.push_back(msg("tool", "<skill>\nreview\nSKILL body"));
    messages.push_back(msg("system", "system notice"));
    messages.push_back(msg("assistant", "Done: switched to pnpm."));

    const auto items = acecode::collect_memory_transcript(messages, 1);
    ASSERT_EQ(items.size(), 4u);
    EXPECT_EQ(items[0].kind, acecode::MemoryTranscriptKind::User);
    EXPECT_EQ(items[0].text, "please always use pnpm");
    EXPECT_EQ(items[1].kind, acecode::MemoryTranscriptKind::AssistantIntermediate);
    EXPECT_EQ(items[2].kind, acecode::MemoryTranscriptKind::Tool);
    EXPECT_EQ(items[3].kind, acecode::MemoryTranscriptKind::AssistantFinal);
}

// 场景:输入超出预算,且有一条 10 KB 的工具输出。
// 期望:工具输出先截到 2 KB;仍超时依次丢工具输出、中间说明、最终结论,用户原话保留到
// 最后;保留下来的按原顺序渲染,并注明省略了几条。
TEST(MemorySummaryTest, TranscriptTrimsByPriority) {
    using Kind = acecode::MemoryTranscriptKind;
    std::vector<acecode::MemoryTranscriptItem> items = {
        {Kind::User, "user question one"},
        {Kind::Tool, std::string(10 * 1024, 'x')},
        {Kind::AssistantIntermediate, "intermediate note"},
        {Kind::AssistantFinal, "final answer"},
        {Kind::User, "user question two"},
    };
    const std::string roomy = acecode::render_memory_transcript(items, 1'000'000);
    EXPECT_NE(roomy.find("[... tool output truncated]"), std::string::npos);
    EXPECT_LT(roomy.size(), 3000u);

    const std::string tight = acecode::render_memory_transcript(items, 120);
    EXPECT_EQ(tight.find("xxxx"), std::string::npos);
    EXPECT_EQ(tight.find("intermediate note"), std::string::npos);
    EXPECT_NE(tight.find("user question one"), std::string::npos) << tight;
    EXPECT_NE(tight.find("user question two"), std::string::npos) << tight;
    EXPECT_NE(tight.find("omitted to fit the budget"), std::string::npos);
    EXPECT_LT(tight.find("user question one"), tight.find("user question two"));
}

// 场景:模型返回 noop。期望:解析成功、没有观察。
TEST(MemorySummaryTest, ParsesNoop) {
    const auto parsed = acecode::parse_memory_extraction_output(R"({"outcome":"noop","observations":[]})");
    EXPECT_TRUE(parsed.ok);
    EXPECT_TRUE(parsed.noop);
}

// 场景:模型返回两条合法观察(其中一份包在 ```json 围栏里)。
// 期望:解析出作用域、类型、标题、陈述;围栏被剥掉。
TEST(MemorySummaryTest, ParsesObservationsInsideCodeFence) {
    const auto parsed = acecode::parse_memory_extraction_output(
        "```json\n{\"outcome\":\"observations\",\"observations\":["
        "{\"scope\":\"global\",\"type\":\"feedback\",\"title\":\"Use pnpm\",\"statement\":\"The user wants pnpm.\"},"
        "{\"scope\":\"workspace\",\"type\":\"project\",\"title\":\"Build dir\",\"statement\":\"Build into L:/build.\"}"
        "]}\n```");
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.observations.size(), 2u);
    EXPECT_EQ(parsed.observations[0].scope, acecode::MemoryScope::Global);
    EXPECT_EQ(parsed.observations[1].type, acecode::MemoryType::Project);
}

// 场景:输出不合规 —— 观察多出 priority 字段、非法作用域、标题超 80 字符、陈述为空、
// 超过 20 条、noop 却带观察、顶层多字段、不是 JSON。
// 期望:每一种都整份作废(ok=false)。
TEST(MemorySummaryTest, RejectsNonConformingOutput) {
    auto obs = [](const std::string& extra) {
        return std::string(R"({"outcome":"observations","observations":[{"scope":"global","type":"user","title":"t","statement":"s")") +
               extra + "}]}";
    };
    EXPECT_FALSE(acecode::parse_memory_extraction_output(obs(R"(,"priority":1)")).ok);
    EXPECT_FALSE(acecode::parse_memory_extraction_output(
        R"({"outcome":"observations","observations":[{"scope":"team","type":"user","title":"t","statement":"s"}]})").ok);
    EXPECT_FALSE(acecode::parse_memory_extraction_output(
        R"({"outcome":"observations","observations":[{"scope":"global","type":"user","title":")" +
        std::string(81, 'a') + R"(","statement":"s"}]})").ok);
    EXPECT_FALSE(acecode::parse_memory_extraction_output(
        R"({"outcome":"observations","observations":[{"scope":"global","type":"user","title":"t","statement":" "}]})").ok);
    nlohmann::json many = {{"outcome", "observations"}, {"observations", nlohmann::json::array()}};
    for (int i = 0; i < 21; ++i) {
        many["observations"].push_back({{"scope", "global"}, {"type", "user"}, {"title", "t"}, {"statement", "s"}});
    }
    EXPECT_FALSE(acecode::parse_memory_extraction_output(many.dump()).ok);
    EXPECT_FALSE(acecode::parse_memory_extraction_output(
        R"({"outcome":"noop","observations":[{"scope":"global","type":"user","title":"t","statement":"s"}]})").ok);
    EXPECT_FALSE(acecode::parse_memory_extraction_output(R"({"outcome":"noop","observations":[],"note":"x"})").ok);
    EXPECT_FALSE(acecode::parse_memory_extraction_output("I could not find anything.").ok);
}

// 场景:标题恰好 80 个中文字符(UTF-8 下 240 字节)。期望:按字符计长度,合法。
TEST(MemorySummaryTest, LengthLimitsCountCharactersNotBytes) {
    std::string title;
    for (int i = 0; i < 80; ++i) title += u8"记";
    nlohmann::json j = {{"outcome", "observations"},
                        {"observations", {{{"scope", "global"}, {"type", "user"}, {"title", title}, {"statement", "s"}}}}};
    EXPECT_TRUE(acecode::parse_memory_extraction_output(j.dump()).ok);
}

// 场景:合法计划 —— create 新条目、update 摘要条目、merge 两条摘要条目、delete 摘要条目,
// 每个操作都引用本批观察。期望:校验通过。
TEST(MemorySummaryTest, ValidPlanPasses) {
    const auto context = context_with({entry("old", "summary"), entry("a", "summary"),
                                       entry("b", "summary"), entry("stale", "summary")});
    EXPECT_EQ(validate(R"({"operations":[
        {"op":"create","name":"new_rule","type":"feedback","description":"Use pnpm","body":"Details","evidence":["s-0-4#0"]},
        {"op":"update","name":"old","description":"Updated","body":"New body","evidence":["s-0-4#1"]},
        {"op":"merge","name":"a","sources":["a","b"],"type":"project","description":"Merged","body":"Both","evidence":["s-0-4#0"]},
        {"op":"delete","name":"stale","evidence":["s-0-4#1"]}]})", context), "");
    EXPECT_EQ(validate(R"({"operations":[]})", context), "");
}

// 场景:计划中某个 update 没有引用任何观察 / 引用了不在本批的观察。期望:整份作废。
TEST(MemorySummaryTest, PlanWithoutEvidenceIsRejected) {
    const auto context = context_with({entry("old", "summary")});
    EXPECT_NE(validate(R"({"operations":[{"op":"update","name":"old","description":"d","body":"b","evidence":[]}]})",
                       context).find("evidence is required"), std::string::npos);
    EXPECT_NE(validate(R"({"operations":[{"op":"update","name":"old","description":"d","body":"b","evidence":["x#9"]}]})",
                       context).find("not in this batch"), std::string::npos);
}

// 场景:计划试图删除 / 更新 / 合并 source: manual 的条目(没有 source 的旧条目也算手写)。
// 期望:整份作废,手写条目受保护。
TEST(MemorySummaryTest, ManualEntriesAreReadOnly) {
    const auto context = context_with({entry("hand", "manual"), entry("legacy", ""), entry("auto", "summary")});
    EXPECT_NE(validate(R"({"operations":[{"op":"delete","name":"hand","evidence":["s-0-4#0"]}]})", context)
                  .find("read-only"), std::string::npos);
    EXPECT_NE(validate(R"({"operations":[{"op":"update","name":"legacy","description":"d","body":"b","evidence":["s-0-4#0"]}]})",
                       context).find("read-only"), std::string::npos);
    EXPECT_NE(validate(R"({"operations":[{"op":"merge","name":"auto","sources":["auto","hand"],"type":"user","description":"d","body":"b","evidence":["s-0-4#0"]}]})",
                       context).find("read-only"), std::string::npos);
}

// 场景:用户删过条目 old_rule(墓碑),计划试图 create 同名或标题相同(忽略大小写)的条目。
// 期望:整份作废,不会复活。
TEST(MemorySummaryTest, TombstonesBlockRecreation) {
    auto context = context_with({});
    context.tombstones.push_back({"workspace:x", "old_rule", acecode::normalize_memory_title("Prefer Tabs"), 0});
    EXPECT_NE(validate(R"({"operations":[{"op":"create","name":"old_rule","type":"user","description":"d","body":"b","evidence":["s-0-4#0"]}]})",
                       context).find("deleted this memory"), std::string::npos);
    EXPECT_NE(validate(R"({"operations":[{"op":"create","name":"fresh","type":"user","description":"prefer  TABS","body":"b","evidence":["s-0-4#0"]}]})",
                       context).find("deleted this memory"), std::string::npos);
}

// 场景:名字非法、描述超 150 字符、正文超 4 KiB、同一条目被两个操作触及、create 已存在
// 的名字、未知操作、操作带多余字段。期望:各自作废。
TEST(MemorySummaryTest, PlanStructuralRules) {
    const auto context = context_with({entry("taken", "summary")});
    EXPECT_FALSE(validate(R"({"operations":[{"op":"create","name":"../x","type":"user","description":"d","body":"b","evidence":["s-0-4#0"]}]})", context).empty());
    EXPECT_FALSE(validate(R"({"operations":[{"op":"create","name":"long","type":"user","description":")" +
                          std::string(151, 'd') + R"(","body":"b","evidence":["s-0-4#0"]}]})", context).empty());
    EXPECT_FALSE(validate(R"({"operations":[{"op":"create","name":"big","type":"user","description":"d","body":")" +
                          std::string(4097, 'b') + R"(","evidence":["s-0-4#0"]}]})", context).empty());
    EXPECT_FALSE(validate(R"({"operations":[
        {"op":"update","name":"taken","description":"d","body":"b","evidence":["s-0-4#0"]},
        {"op":"delete","name":"taken","evidence":["s-0-4#1"]}]})", context).empty());
    EXPECT_FALSE(validate(R"({"operations":[{"op":"create","name":"taken","type":"user","description":"d","body":"b","evidence":["s-0-4#0"]}]})", context).empty());
    EXPECT_FALSE(acecode::parse_memory_consolidation_plan(R"({"operations":[{"op":"rename","name":"a","evidence":["s-0-4#0"]}]})").ok);
    EXPECT_FALSE(acecode::parse_memory_consolidation_plan(R"({"operations":[{"op":"delete","name":"a","evidence":["s-0-4#0"],"why":"x"}]})").ok);
    EXPECT_FALSE(acecode::parse_memory_consolidation_plan(R"({"operations":[],"summary":"x"})").ok);
}

// 场景:同一份计划算两次 hash;换一个字符。期望:稳定且能区分。
TEST(MemorySummaryTest, PlanHashIsStable) {
    EXPECT_EQ(acecode::memory_plan_hash("{\"operations\":[]}"), acecode::memory_plan_hash("{\"operations\":[]}"));
    EXPECT_NE(acecode::memory_plan_hash("a"), acecode::memory_plan_hash("b"));
    EXPECT_EQ(acecode::memory_plan_hash("a").size(), 16u);
}

// 场景:模型把推理过程写进正文(或包在 <think> 里),之后才输出要求的 JSON。
// 期望:去掉推理块与前置文字后仍能解析;字段校验不放宽 —— 观察多出字段照样作废。
// 回归背景:端到端手测时一个推理模型的回复以「The user is giving me...」开头,整份
// 提炼被判成「不是合法 JSON」,记忆摘要从未产出任何观察。
TEST(MemorySummaryTest, ToleratesReasoningAroundJson) {
    const auto prose = acecode::parse_memory_extraction_output(
        "The user stated a team rule. I should record it.\n"
        "{\"outcome\":\"observations\",\"observations\":[{\"scope\":\"workspace\",\"type\":\"project\","
        "\"title\":\"Release on Fridays\",\"statement\":\"Releases only happen on Fridays.\"}]}\nDone.");
    ASSERT_TRUE(prose.ok) << prose.error;
    EXPECT_EQ(prose.observations.size(), 1u);

    const auto think = acecode::parse_memory_extraction_output(
        "<think>{maybe} nothing</think>\n{\"outcome\":\"noop\",\"observations\":[]}");
    ASSERT_TRUE(think.ok) << think.error;
    EXPECT_TRUE(think.noop);

    const auto plan = acecode::parse_memory_consolidation_plan(
        "Plan follows:\n```json\n{\"operations\":[]}\n```");
    EXPECT_TRUE(plan.ok) << plan.error;

    EXPECT_FALSE(acecode::parse_memory_extraction_output(
        "Here: {\"outcome\":\"observations\",\"observations\":[{\"scope\":\"global\",\"type\":\"user\","
        "\"title\":\"t\",\"statement\":\"s\",\"priority\":1}]}").ok);
}
