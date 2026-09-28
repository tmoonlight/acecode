#include "test_support/agent/agent_loop_fixture.hpp"
// 覆盖 AgentLoop 的模型族适配(openspec add-gpt-apply-patch-adaptation):
//   1. GPT 系模型:请求工具表里有 apply_patch、没有 file_edit / file_write,
//      系统提示走 apply_patch 指引与模型族段
//   2. 非 GPT 模型:工具表与提示保持改动前形态(有 file_edit / file_write、无 apply_patch)
//   3. 中途切模型:同一个 AgentLoop 下一轮请求按新模型裁表,ToolExecutor 不重建
//      (三个工具始终 has_tool)
// 用真实的 register_session_builtin_tools 注册工具,StubLlmProvider 记录每轮请求。

#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "config/config.hpp"
#include "permissions/permissions.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/builtin_tool_registry.hpp"
#include "tool/tool_executor.hpp"
#include "llm/tool_protocol_names.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path make_temp_dir() {
    static std::atomic<unsigned int> sequence{0};
    // 目录名里不能带 apply_patch / file_edit:cwd 会原样进 system prompt 的
    // # Environment,下面的「提示里不出现某工具名」断言会被路径本身打穿。
    auto path = fs::temp_directory_path() /
        ("acecode_model_family_loop_" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
         std::to_string(sequence.fetch_add(1)));
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}

class ApplyPatchAgentHarness {
public:
    explicit ApplyPatchAgentHarness(std::string cwd, const std::string& model)
        : cwd_(std::move(cwd)) {
        provider_ = std::make_shared<acecode_test::StubLlmProvider>();
        provider_->set_model(model);

        acecode::AppConfig config;
        config.web_search.enabled = false;
        acecode::register_session_builtin_tools(tools_, config);

        acecode::AgentCallbacks callbacks;
        callbacks.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lock(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> {
            return provider_;
        };
        loop_ = std::make_unique<acecode::AgentLoop>(
        acecode_test::AgentLoopFixture::dependencies(accessor, tools_, callbacks, permissions_),
        acecode_test::AgentLoopFixture::configuration(cwd_));
        loop_->start();
    }

    ~ApplyPatchAgentHarness() {
        if (loop_) loop_->shutdown();
    }

    acecode_test::StubLlmProvider& provider() { return *provider_; }
    acecode::ToolExecutor& tools() { return tools_; }

    bool run_text_turn(std::chrono::milliseconds timeout = 10s) {
        provider_->push_text("done");
        {
            std::lock_guard<std::mutex> lock(busy_mu_);
            busy_ = true;
        }
        loop_->submit("hello");
        std::unique_lock<std::mutex> lock(busy_mu_);
        return busy_cv_.wait_for(lock, timeout, [this] { return !busy_; });
    }

private:
    acecode::ScopedModelToolNameMappings none_{{}};
    std::string cwd_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_;
    acecode::ToolExecutor tools_;
    acecode::PermissionManager permissions_;
    std::unique_ptr<acecode::AgentLoop> loop_;
    std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
};

std::vector<std::string> tool_names(const std::vector<acecode::ToolDef>& defs) {
    std::vector<std::string> names;
    for (const auto& def : defs) names.push_back(def.name);
    return names;
}

bool has(const std::vector<std::string>& names, const char* name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::string system_prompt_of(const std::vector<acecode::ChatMessage>& messages) {
    for (const auto& message : messages) {
        if (message.role == "system") return message.content;
    }
    return {};
}

} // namespace

// 场景:模型 id 为 gpt-5,发一轮纯文本请求。
// 期望:第一轮请求的工具表含 apply_patch、不含 file_edit / file_write(file_read /
// bash 等照常);系统提示含 apply_patch 指引与 "# Model-specific guidance",不含
// file_edit / file_write 字样;ToolExecutor 里三个工具仍然都注册着。
TEST(AgentLoopApplyPatch, GptModelGetsApplyPatchInsteadOfFileEditTools) {
    const fs::path cwd = make_temp_dir();
    ApplyPatchAgentHarness harness(cwd.string(), "gpt-5");
    ASSERT_TRUE(harness.run_text_turn());

    const auto names = tool_names(harness.provider().tools_for_turn(0));
    ASSERT_FALSE(names.empty());
    EXPECT_TRUE(has(names, "apply_patch"));
    EXPECT_FALSE(has(names, "file_edit"));
    EXPECT_FALSE(has(names, "file_write"));
    EXPECT_TRUE(has(names, "file_read"));
    EXPECT_TRUE(has(names, "bash"));

    const std::string prompt = system_prompt_of(harness.provider().messages_for_turn(0));
    ASSERT_FALSE(prompt.empty());
    EXPECT_NE(prompt.find("`apply_patch` for every file creation"), std::string::npos);
    EXPECT_NE(prompt.find("# Model-specific guidance"), std::string::npos);
    EXPECT_EQ(prompt.find("file_edit"), std::string::npos);
    EXPECT_EQ(prompt.find("file_write"), std::string::npos);

    EXPECT_TRUE(harness.tools().has_tool("apply_patch"));
    EXPECT_TRUE(harness.tools().has_tool("file_edit"));
    EXPECT_TRUE(harness.tools().has_tool("file_write"));

    fs::remove_all(cwd);
}

// 场景:模型 id 为 claude-sonnet-4。
// 期望:工具表含 file_edit / file_write、不含 apply_patch;系统提示不含
// apply_patch 指引与模型族段 —— 非 GPT 模型的行为与改动前一致。
TEST(AgentLoopApplyPatch, NonGptModelKeepsFileEditTools) {
    const fs::path cwd = make_temp_dir();
    ApplyPatchAgentHarness harness(cwd.string(), "claude-sonnet-4");
    ASSERT_TRUE(harness.run_text_turn());

    const auto names = tool_names(harness.provider().tools_for_turn(0));
    ASSERT_FALSE(names.empty());
    EXPECT_FALSE(has(names, "apply_patch"));
    EXPECT_TRUE(has(names, "file_edit"));
    EXPECT_TRUE(has(names, "file_write"));

    const std::string prompt = system_prompt_of(harness.provider().messages_for_turn(0));
    ASSERT_FALSE(prompt.empty());
    const std::size_t leak = prompt.find("apply_patch");
    EXPECT_EQ(leak, std::string::npos)
        << "system prompt mentions apply_patch near: "
        << prompt.substr(leak > 200 ? leak - 200 : 0, 400);
    EXPECT_EQ(prompt.find("# Model-specific guidance"), std::string::npos);
    EXPECT_NE(prompt.find("`file_edit` will error"), std::string::npos);

    fs::remove_all(cwd);
}

// 场景:同一会话第一轮是 Claude,第二轮之前把 provider 切成 gpt-5-codex。
// 期望:第二轮请求的工具表按 GPT 规则裁(有 apply_patch、无 file_edit),
// 第一轮仍是 Claude 形态 —— 裁表发生在每次组装请求时,不依赖启动时的模型。
TEST(AgentLoopApplyPatch, SwitchingModelMidSessionReshapesNextRequest) {
    const fs::path cwd = make_temp_dir();
    ApplyPatchAgentHarness harness(cwd.string(), "claude-sonnet-4");
    ASSERT_TRUE(harness.run_text_turn());
    harness.provider().set_model("gpt-5-codex");
    ASSERT_TRUE(harness.run_text_turn());

    const auto first = tool_names(harness.provider().tools_for_turn(0));
    const auto second = tool_names(harness.provider().tools_for_turn(1));
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_TRUE(has(first, "file_edit"));
    EXPECT_FALSE(has(first, "apply_patch"));
    EXPECT_TRUE(has(second, "apply_patch"));
    EXPECT_FALSE(has(second, "file_edit"));
    EXPECT_FALSE(has(second, "file_write"));

    fs::remove_all(cwd);
}
