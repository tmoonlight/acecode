#include <gtest/gtest.h>
#include <algorithm>
#include "test_support/agent_loop/characterization_fixture.hpp"
#include "experts/expert_registry.hpp"
#include "skills/skill_registry.hpp"
#include "utils/scope_exit.hpp"

namespace {
using namespace acecode_test::characterization;
bool contains(const std::vector<acecode::ChatMessage>& messages, const std::string& text) {
    return std::any_of(messages.begin(), messages.end(), [&](const auto& message) {
        return message.content.find(text) != std::string::npos;
    });
}
std::shared_ptr<const acecode::ExpertDefinition> expert(const std::string& name) {
    acecode::ExpertDefinition value;
    value.id = name; value.version = "1.0"; value.display_name = name;
    value.lead_agent_id = name;
    value.agents = {{name, name, "test", name + " instructions", {}, {}}};
    return std::make_shared<const acecode::ExpertDefinition>(std::move(value));
}
std::shared_ptr<const acecode::SkillRegistry> skills(
    const std::filesystem::path& directory, const std::string& name) {
    const auto root = directory / name;
    std::filesystem::create_directories(root);
    std::ofstream(root / "SKILL.md") << "---\nname: " << name
        << "\ndescription: " << name << " instruction set\n---\nUse " << name << ".\n";
    acecode::SkillRegistry registry;
    registry.set_scan_roots({root});
    return registry.snapshot();
}
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    void release() {
        { std::lock_guard<std::mutex> lock(mutex); released = true; }
        changed.notify_all();
    }
};
}

// 场景：工具挂起时排队切换专家及技能。期望旧回合全程保留旧集合，
// control 发布后的下一回合才用新集合；防止注册表替换使旧回合悬垂或混用权限。
TEST(PromptConfigSnapshot, ExpertAndSkillsSwitchAtTheFollowingTurn) {
    Isolation isolation;
    Harness harness(isolation);
    auto before = skills(harness.cwd, "before-skill");
    auto after = skills(harness.cwd, "after-skill");
    harness.loop->publish_expert_snapshot(expert("EXPERT_BEFORE"), before, {});
    auto gate = std::make_shared<Gate>();
    auto tool = harness.probe("hold", true);
    tool.execute = [gate](const std::string&, const acecode::ToolContext&) {
        std::unique_lock<std::mutex> lock(gate->mutex);
        gate->entered = true; gate->changed.notify_all();
        const bool released = gate->changed.wait_for(lock, 3s, [gate] { return gate->released; });
        return acecode::ToolResult{"released", released};
    };
    harness.tools.register_tool(std::move(tool));
    harness.provider->push_tool_call("hold", "{}");
    harness.provider->push_text("first complete");
    acecode::ScopeExit release([gate] { gate->release(); });
    harness.loop->submit("first");
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        ASSERT_TRUE(gate->changed.wait_for(lock, 2s, [gate] { return gate->entered; }));
    }
    // fixture 必定先 shutdown 再析构，测试控制项的借用不逃出 fixture。
    auto receipt = harness.loop->enqueue_control(
        [loop = harness.loop.get(), replacement = expert("EXPERT_AFTER"), after] {
            loop->publish_expert_snapshot(replacement, after, {});
            return true;
        });
    ASSERT_TRUE(receipt.accepted);
    EXPECT_TRUE(receipt.queued_behind_turn);
    gate->release();
    ASSERT_TRUE(receipt.wait_for_completion(3s));
    ASSERT_TRUE(receipt.applied());
    harness.provider->push_text("second complete");
    ASSERT_TRUE(harness.perform([&harness] { harness.loop->submit("second"); }));
    auto first = harness.provider->messages_for_turn(0);
    auto continued = harness.provider->messages_for_turn(1);
    auto next = harness.provider->messages_for_turn(2);
    EXPECT_TRUE(contains(first, "EXPERT_BEFORE"));
    EXPECT_TRUE(contains(continued, "EXPERT_BEFORE"));
    EXPECT_TRUE(contains(continued, "before-skill"));
    EXPECT_FALSE(contains(continued, "EXPERT_AFTER"));
    EXPECT_FALSE(contains(continued, "after-skill"));
    EXPECT_TRUE(contains(next, "EXPERT_AFTER"));
    EXPECT_TRUE(contains(next, "after-skill"));
    EXPECT_FALSE(contains(next, "EXPERT_BEFORE"));
}

// 场景：空闲时发布设置后 prime 侧问上下文。期望每次 prime 只取一次当前快照，
// 后续发布不修改已发布的侧问消息；此前可变 AppConfig 引用可能跨读取混用。
TEST(PromptConfigSnapshot, IdleSideQuestionPrimeCapturesOneIndependentConfiguration) {
    Isolation isolation;
    struct State { std::string text = "PRIME_BEFORE"; int captures = 0; };
    auto state = std::make_shared<State>();
    Harness harness(isolation, "prime", {}, true, [state] {
        ++state->captures;
        acecode::SessionPromptConfig config;
        config.custom_instructions.emplace();
        config.custom_instructions->set_text(state->text);
        return config;
    });
    harness.loop->prime_side_question_context();
    EXPECT_EQ(state->captures, 1);
    const auto before = harness.loop->side_question_context_snapshot();
    EXPECT_TRUE(contains(before, "PRIME_BEFORE"));
    state->text = "PRIME_AFTER";
    EXPECT_TRUE(contains(harness.loop->side_question_context_snapshot(), "PRIME_BEFORE"));
    harness.loop->prime_side_question_context();
    EXPECT_EQ(state->captures, 2);
    EXPECT_TRUE(contains(harness.loop->side_question_context_snapshot(), "PRIME_AFTER"));
    EXPECT_TRUE(contains(before, "PRIME_BEFORE"));
}
