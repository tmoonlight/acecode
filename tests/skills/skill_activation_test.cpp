// Codex-compatible explicit Skill selection:
// - `$SkillName` and linked `[$SkillName](SKILL.md path)` parsing
// - common environment-variable exclusion
// - path-first matching, registry-order output, and deduplication
// - complete SKILL.md injection through user-role `<skill>` fragments
// - slash-command mapping onto the same linked-mention path

#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "skills/skill_activation.hpp"
#include "skills/skill_registry.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void write_skill(const fs::path& root,
                 const std::string& name,
                 const std::string& description,
                 const std::string& body) {
    const fs::path dir = root / acecode::path_from_utf8(name);
    fs::create_directories(dir);
    std::ofstream out(dir / "SKILL.md", std::ios::binary);
    out << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n"
        << body << "\n";
}

class SkillActivationTest : public ::testing::Test {
protected:
    fs::path root;
    acecode::SkillRegistry registry;

    void SetUp() override {
        std::random_device rd;
        root = fs::temp_directory_path() /
               ("acecode-skill-activation-" + std::to_string(rd()));
        fs::create_directories(root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void scan() {
        registry.set_scan_roots({root});
        registry.scan();
    }
};

class CapturingSkillProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>& messages,
        const std::vector<acecode::ToolDef>&) override {
        record(messages);
        acecode::ChatResponse response;
        response.content = "ok";
        response.finish_reason = "stop";
        return response;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>& messages,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* = nullptr) override {
        record(messages);
        acecode::StreamEvent delta;
        delta.type = acecode::StreamEventType::Delta;
        delta.content = "ok";
        callback(delta);
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        callback(done);
    }

    bool wait_for_count(std::size_t expected,
                        std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, timeout, [&] {
            return requests_.size() >= expected;
        });
    }

    std::vector<acecode::ChatMessage> request(std::size_t index) const {
        std::lock_guard<std::mutex> lock(mu_);
        return index < requests_.size()
            ? requests_[index]
            : std::vector<acecode::ChatMessage>{};
    }

    std::string name() const override { return "capturing-skill-provider"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "capturing-skill-provider"; }
    void set_model(const std::string&) override {}

private:
    void record(const std::vector<acecode::ChatMessage>& messages) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            requests_.push_back(messages);
        }
        cv_.notify_all();
    }

    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    std::vector<std::vector<acecode::ChatMessage>> requests_;
};

TEST_F(SkillActivationTest, DollarMentionInjectsCompleteSkillOnce) {
    write_skill(root, "review-pr", "Review pull requests",
                "# Review workflow\nUse the required checklist.");
    scan();

    const std::string request =
        "Use $review-pr and mention $review-pr again for this change.";
    const auto expanded =
        acecode::inject_explicit_skill_instructions(request, registry);

    ASSERT_EQ(expanded.injected_skill_names,
              (std::vector<std::string>{"review-pr"}));
    EXPECT_EQ(expanded.prompt.rfind(request, 0), 0u);
    EXPECT_NE(expanded.prompt.find("<skill>"), std::string::npos);
    EXPECT_NE(expanded.prompt.find("<name>review-pr</name>"), std::string::npos);
    EXPECT_NE(expanded.prompt.find("<path>"), std::string::npos);
    EXPECT_NE(expanded.prompt.find("name: review-pr"), std::string::npos)
        << "frontmatter must be included, matching Codex host Skill injection";
    EXPECT_NE(expanded.prompt.find("Use the required checklist."),
              std::string::npos);
    EXPECT_EQ(expanded.prompt.find("<skill>", expanded.prompt.find("<skill>") + 1),
              std::string::npos);
}

TEST_F(SkillActivationTest, LinkedWindowsStylePathSelectsByPath) {
    write_skill(root, "linked-skill", "Linked", "Linked body");
    scan();
    const auto meta = registry.find("linked-skill");
    ASSERT_TRUE(meta.has_value());
    std::string windows_path =
        acecode::path_to_utf8_generic(meta->skill_md_path);
    std::replace(windows_path.begin(), windows_path.end(), '/', '\\');

    const std::string request =
        "Use [$different-label](" + windows_path + ") now.";
    const auto selected =
        acecode::collect_explicit_skill_mentions(request, registry);

    ASSERT_EQ(selected.size(), 1u);
    EXPECT_EQ(selected.front().name, "linked-skill");
}

TEST_F(SkillActivationTest, CommonEnvironmentVariablesNeverSelectSkills) {
    write_skill(root, "PATH", "Must not activate", "Unsafe false positive");
    write_skill(root, "HOME", "Must not activate", "Unsafe false positive");
    scan();

    const std::string request = "Print $PATH and then inspect $HOME.";
    const auto expanded =
        acecode::inject_explicit_skill_instructions(request, registry);

    EXPECT_TRUE(expanded.injected_skill_names.empty());
    EXPECT_EQ(expanded.prompt, request);
}

TEST_F(SkillActivationTest, AppResourceLinkDoesNotFallBackToSkillName) {
    write_skill(root, "calendar", "Local calendar Skill", "Local body");
    scan();

    const std::string request = "Use [$calendar](app://calendar) today.";
    const auto expanded =
        acecode::inject_explicit_skill_instructions(request, registry);

    EXPECT_TRUE(expanded.injected_skill_names.empty());
    EXPECT_EQ(expanded.prompt, request);
}

TEST_F(SkillActivationTest, MultipleMentionsPreserveRegistryOrder) {
    write_skill(root, "beta", "Beta", "Beta body");
    write_skill(root, "alpha", "Alpha", "Alpha body");
    scan();

    const auto expanded = acecode::inject_explicit_skill_instructions(
        "$beta then $alpha", registry);

    ASSERT_EQ(expanded.injected_skill_names,
              (std::vector<std::string>{"alpha", "beta"}));
    const auto alpha = expanded.prompt.find("<name>alpha</name>");
    const auto beta = expanded.prompt.find("<name>beta</name>");
    ASSERT_NE(alpha, std::string::npos);
    ASSERT_NE(beta, std::string::npos);
    EXPECT_LT(alpha, beta);
}

TEST_F(SkillActivationTest, SlashMappingReusesLinkedMentionInjection) {
    write_skill(root, "calculator", "Calculate", "Follow calculator rules.");
    scan();
    const auto meta = registry.find("calculator");
    ASSERT_TRUE(meta.has_value());

    const std::string mention =
        acecode::build_skill_invocation_hint(*meta, "  33 * 44  ");
    EXPECT_EQ(mention.rfind("[$calculator](", 0), 0u);
    EXPECT_NE(mention.find("\n\n33 * 44"), std::string::npos);

    const auto expanded =
        acecode::inject_explicit_skill_instructions(mention, registry);
    ASSERT_EQ(expanded.injected_skill_names,
              (std::vector<std::string>{"calculator"}));
    EXPECT_NE(expanded.prompt.find("Follow calculator rules."),
              std::string::npos);
}

TEST_F(SkillActivationTest, NoMentionKeepsPromptByteIdentical) {
    write_skill(root, "review", "Review", "Review body");
    scan();
    const std::string request = "Please explain the current code.";

    const auto expanded =
        acecode::inject_explicit_skill_instructions(request, registry);

    EXPECT_TRUE(expanded.injected_skill_names.empty());
    EXPECT_EQ(expanded.prompt, request);
}

TEST_F(SkillActivationTest, AgentLoopInjectsOnlyTheMentioningTurnAndKeepsDisplayText) {
    write_skill(root, "review", "Review", "Follow the injected review workflow.");
    scan();

    auto provider = std::make_shared<CapturingSkillProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentCallbacks callbacks;
    acecode::AgentLoop loop(
        [provider]() -> std::shared_ptr<acecode::LlmProvider> {
            return provider;
        },
        tools, callbacks, acecode::path_to_utf8(root), permissions);
    loop.set_skill_registry(&registry);

    acecode::UserInput input;
    input.text = "$review inspect this attachment";
    input.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", input.text}},
        nlohmann::json{{"type", "browser_context"},
                       {"context", nlohmann::json{{"url", "https://example.test"}}}},
    });
    loop.submit(input);
    ASSERT_TRUE(provider->wait_for_count(1, std::chrono::seconds(5)));

    const auto first_request = provider->request(0);
    auto first_user = std::find_if(
        first_request.begin(), first_request.end(), [&](const auto& message) {
            return message.role == "user" && message.metadata.is_object() &&
                   message.metadata.value("display_text", std::string{}) == input.text;
        });
    ASSERT_NE(first_user, first_request.end());
    EXPECT_NE(first_user->content.find("<name>review</name>"), std::string::npos);
    ASSERT_TRUE(first_user->content_parts.is_array());
    ASSERT_FALSE(first_user->content_parts.empty());
    EXPECT_NE(first_user->content_parts[0].value("text", std::string{})
                  .find("<name>review</name>"),
              std::string::npos)
        << "structured attachment requests must carry the injected prompt too";

    loop.submit("This second turn has no explicit Skill mention.");
    ASSERT_TRUE(provider->wait_for_count(2, std::chrono::seconds(5)));
    const auto second_request = provider->request(1);
    auto second_user = std::find_if(
        second_request.rbegin(), second_request.rend(), [](const auto& message) {
            return message.role == "user" &&
                   message.content.find("This second turn has no explicit") !=
                       std::string::npos;
        });
    ASSERT_NE(second_user, second_request.rend());
    EXPECT_EQ(second_user->content.find("<skill>"), std::string::npos)
        << "an explicit selection must not be automatically re-injected next turn";
}

} // namespace
