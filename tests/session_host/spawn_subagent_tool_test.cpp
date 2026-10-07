// 覆盖 src/tool/spawn_subagent_tool.cpp。
//
// spawn_subagent / wait_subagent 是 daemon 的子代理工具:在 SessionRegistry
// 里创建独立子会话并注入首条消息,父会话上下文只吃回最终摘要。一旦回归:
//   - 深度限制失效 → 子代理递归派生,会话数失控
//   - wait 判定失效 → 父会话死等 / 提前返回空结果
//   - deps 未回填时崩溃(而不是报"仅 daemon 可用")
//
// 测试不真跑 LLM:EchoStreamProvider 的 chat_stream 立即产出固定文本,
// 让子会话 turn 有真实的 assistant 消息可供 wait 逻辑取回。

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "experts/expert_registry.hpp"
#include "permissions/permissions.hpp"
#include "session_host/local_session_client.hpp"
#include "session_host/session_registry.hpp"
#include "session/session_storage.hpp"
#include "session_host/tools/spawn_subagent_tool.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"
#include "worktree/worktree_manager.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

void write_test_skill(const fs::path& workspace,
                      const std::string& name,
                      const std::string& description) {
    const fs::path dir = workspace / ".acecode" / "skills" / name;
    fs::create_directories(dir);
    std::ofstream out(dir / "SKILL.md", std::ios::binary);
    out << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n"
        << "# " << name << "\n";
}

// 立即回复固定文本的 stub:子会话 turn 会产生一条 assistant 消息。
class EchoStreamProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>&,
        const std::vector<acecode::ToolDef>&) override {
        acecode::ChatResponse resp;
        resp.content = "subagent-final-reply";
        resp.finish_reason = "stop";
        return resp;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* = nullptr) override {
        acecode::StreamEvent delta;
        delta.type = acecode::StreamEventType::Delta;
        delta.content = "subagent-final-reply";
        callback(delta);
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        callback(done);
    }

    std::string name() const override { return "echo-stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "echo-stub"; }
    void set_model(const std::string&) override {}
};

class RecordingSessionClient : public acecode::SessionClient {
public:
    const bool* on_spawn_seen = nullptr;
    bool send_saw_on_spawn = false;
    std::string sent_session_id;
    std::string sent_text;
    std::string sent_display_text;

    std::string create_session(const acecode::SessionOptions&) override { return {}; }
    bool resume_session(const std::string&, const acecode::SessionOptions& = {}) override {
        return false;
    }
    std::vector<acecode::SessionInfo> list_sessions() override { return {}; }
    void destroy_session(const std::string&) override {}
    SubscriptionId subscribe(const std::string&,
                             EventListener,
                             std::uint64_t = 0) override {
        return 0;
    }
    void unsubscribe(const std::string&, SubscriptionId) override {}
    bool send_input(const std::string& session_id,
                    const std::string& text) override {
        return send_input(session_id, text, std::string{});
    }
    bool send_input(const std::string& session_id,
                    const std::string& text,
                    const std::string& display_text) override {
        send_saw_on_spawn = on_spawn_seen && *on_spawn_seen;
        sent_session_id = session_id;
        sent_text = text;
        sent_display_text = display_text;
        return true;
    }
    acecode::BuiltinCommandResult execute_builtin_command(
        const std::string&,
        const acecode::BuiltinCommandRequest&) override {
        return {};
    }
    void respond_permission(const std::string&,
                            const acecode::PermissionDecision&) override {}
    acecode::QuestionResponseStatus respond_question(
        const std::string&,
        const std::string&,
        const acecode::AskUserQuestionResponse&) override {
        return acecode::QuestionResponseStatus::Closed;
    }
    void abort(const std::string&) override {}
};

struct SubagentFixture {
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AppConfig config;
    std::shared_ptr<acecode::LlmProvider> provider;
    fs::path cwd;
    acecode::ExpertRegistry experts;
    acecode::SessionRegistry registry;
    acecode::LocalSessionClient client;
    std::shared_ptr<acecode::SubagentToolDeps> deps;

    explicit SubagentFixture(bool resolve_saved_models = false)
        : cwd(fs::temp_directory_path() /
              ("acecode_subagent_test_" + std::to_string(std::random_device{}()))),
          experts(cwd / "global-experts"),
          registry(make_deps(*this, resolve_saved_models)), client(registry) {
        fs::create_directories(cwd);
        deps = std::make_shared<acecode::SubagentToolDeps>();
        deps->registry = &registry;
        deps->client = &client;
        deps->config = &config;
        tools.register_tool(acecode::create_spawn_subagent_tool(deps));
        tools.register_tool(acecode::create_wait_subagent_tool(deps));
    }

    ~SubagentFixture() {
        std::error_code ec;
        fs::remove_all(cwd, ec);
    }

    static acecode::SessionRegistryDeps make_deps(SubagentFixture& self,
                                                 bool resolve_saved_models) {
        acecode::SessionRegistryDeps d;
        d.provider_accessor = [&self] { return self.provider; };
        d.tools = &self.tools;
        d.cwd = "/tmp/subagent_test_registry";
        d.expert_registry = &self.experts;
        d.template_permissions = &self.permissions;
        if (resolve_saved_models) d.config = &self.config;
        return d;
    }

    void configure_models() {
        config.default_model_name = "daemon-default";
        for (const char* name : {"daemon-default", "parent-current", "explicit-child"}) {
            acecode::ModelProfile profile;
            profile.name = name;
            profile.provider = "openai";
            profile.model = std::string(name) + "-model";
            profile.base_url = "http://127.0.0.1:9/v1";
            profile.api_key = "test-key";
            config.saved_models.push_back(std::move(profile));
        }
    }

    acecode::ToolContext ctx_for(const std::string& session_id) {
        acecode::ToolContext ctx;
        ctx.cwd = cwd.string();
        if (!session_id.empty()) {
            if (auto entry = registry.acquire(session_id)) {
                ctx.session_manager = entry->sm.get();
            }
        }
        return ctx;
    }
};

} // namespace

// 场景: deps 未回填(TUI / registry 尚未构造)→ 报"仅 daemon 可用"而不是
// 解引用空指针崩溃。
TEST(SpawnSubagentTool, RejectsWhenDepsMissing) {
    auto empty = std::make_shared<acecode::SubagentToolDeps>();
    auto tool = acecode::create_spawn_subagent_tool(empty);
    auto r = tool.execute(R"({"prompt":"hi"})", acecode::ToolContext{});
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("daemon"), std::string::npos);
}

// 场景: prompt 缺失 → 参数错误,不创建任何会话。
TEST(SpawnSubagentTool, RejectsEmptyPrompt) {
    SubagentFixture fx;
    auto r = fx.tools.execute("spawn_subagent", R"({})", fx.ctx_for(""));
    EXPECT_FALSE(r.success);
    EXPECT_EQ(fx.registry.size(), 0u);
}

// 场景: wait=false 点火即返 —— 返回 subagent_session_id,registry 里能找到
// 该会话且 subagent_depth=1(供后续深度限制判定)。这是流水线接力的形态。
TEST(SpawnSubagentTool, FireAndForgetCreatesIsolatedSession) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    auto r = fx.tools.execute("spawn_subagent",
                              R"({"prompt":"stage two go","wait":false})",
                              fx.ctx_for(""));
    ASSERT_TRUE(r.success) << r.output;
    ASSERT_TRUE(r.metadata.contains("subagent_session_id"));
    const std::string child_id = r.metadata["subagent_session_id"].get<std::string>();
    ASSERT_FALSE(child_id.empty());

    auto child = fx.registry.acquire(child_id);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->subagent_depth, 1);
    fx.registry.destroy(child_id);
}

// Exercise the actual saved-model resolver with a global default that differs
// from the parent. RecordingSessionClient prevents any provider request.
TEST(SpawnSubagentTool, UnspecifiedModelUsesParentsCurrentSelectionAfterSwitch) {
    SubagentFixture fx(true);
    fx.configure_models();
    RecordingSessionClient recording_client;
    fx.deps->client = &recording_client;
    acecode::SessionOptions parent_options;
    parent_options.cwd = fx.cwd.string();
    parent_options.swarm_mode = "star";
    const auto parent_id = fx.registry.create(parent_options);
    const auto initial = fx.registry.current_model_state(parent_id);
    ASSERT_TRUE(initial.has_value());
    EXPECT_EQ(initial->name, "daemon-default");

    std::string error;
    ASSERT_TRUE(fx.registry.switch_model(
        parent_id, fx.config.saved_models[1], nullptr, &error)) << error;
    for (const char* arguments : {
             R"({"prompt":"inherit","wait":false})",
             R"({"prompt":"inherit empty","wait":false,"model":""})"}) {
        const auto result = fx.tools.execute(
            "spawn_subagent", arguments, fx.ctx_for(parent_id));
        ASSERT_TRUE(result.success) << result.output;
        const auto child_id = result.metadata["subagent_session_id"].get<std::string>();
        const auto state = fx.registry.current_model_state(child_id);
        ASSERT_TRUE(state.has_value());
        EXPECT_EQ(state->name, "parent-current");
        EXPECT_EQ(state->provider, "openai");
        EXPECT_EQ(state->model, "parent-current-model");
        fx.registry.destroy(child_id);
    }
    fx.registry.destroy(parent_id);
}

TEST(SpawnSubagentTool, ExplicitModelOverridesParentsCurrentSelection) {
    SubagentFixture fx(true);
    fx.configure_models();
    RecordingSessionClient recording_client;
    fx.deps->client = &recording_client;
    acecode::SessionOptions parent_options;
    parent_options.cwd = fx.cwd.string();
    parent_options.model_name = "parent-current";
    const auto parent_id = fx.registry.create(parent_options);
    const auto result = fx.tools.execute(
        "spawn_subagent",
        R"({"prompt":"override","wait":false,"model":"explicit-child"})",
        fx.ctx_for(parent_id));
    ASSERT_TRUE(result.success) << result.output;
    const auto child_id = result.metadata["subagent_session_id"].get<std::string>();
    const auto state = fx.registry.current_model_state(child_id);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->name, "explicit-child");
    EXPECT_EQ(state->model, "explicit-child-model");
    fx.registry.destroy(child_id);
    fx.registry.destroy(parent_id);
}

TEST(SpawnSubagentTool, InheritsModelFromTuiParentOutsideRegistry) {
    SubagentFixture fx(true);
    fx.configure_models();
    RecordingSessionClient recording_client;
    fx.deps->client = &recording_client;
    acecode::SessionManager parent;
    parent.start_session(fx.cwd.string(), "openai", "parent-current-model",
                         "", "parent-current", "tui");
    const auto parent_id = parent.ensure_active_session_id();
    ASSERT_FALSE(parent_id.empty());
    EXPECT_EQ(fx.registry.acquire(parent_id), nullptr);
    auto ctx = fx.ctx_for("");
    ctx.session_manager = &parent;
    const auto result = fx.tools.execute(
        "spawn_subagent", R"({"prompt":"external parent","wait":false})", ctx);
    ASSERT_TRUE(result.success) << result.output;
    const auto child_id = result.metadata["subagent_session_id"].get<std::string>();
    const auto state = fx.registry.current_model_state(child_id);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->name, "parent-current");
    EXPECT_EQ(state->model, "parent-current-model");
    fx.registry.destroy(child_id);
}

TEST(SpawnSubagentTool, MissingParentModelRetainsConfiguredDefault) {
    SubagentFixture fx(true);
    fx.configure_models();
    RecordingSessionClient recording_client;
    fx.deps->client = &recording_client;
    acecode::SessionManager legacy_parent;
    legacy_parent.start_session(fx.cwd.string(), "openai", "legacy-model");
    for (auto* parent : {static_cast<acecode::SessionManager*>(nullptr), &legacy_parent}) {
        auto ctx = fx.ctx_for("");
        ctx.session_manager = parent;
        const auto result = fx.tools.execute(
            "spawn_subagent", R"({"prompt":"fallback","wait":false})", ctx);
        ASSERT_TRUE(result.success) << result.output;
        const auto child_id = result.metadata["subagent_session_id"].get<std::string>();
        const auto state = fx.registry.current_model_state(child_id);
        ASSERT_TRUE(state.has_value());
        EXPECT_EQ(state->name, "daemon-default");
        EXPECT_EQ(state->model, "daemon-default-model");
        fx.registry.destroy(child_id);
    }
}

// 场景: daemon 要在子会话首条输入入队前安装 tracking 监听器,否则 child
// worker 可能抢先 emit busy=true / permission_request,主会话 UI 仍然发现不了
// 这个后台任务。用 fake SessionClient 确定性验证 on_spawn 早于 send_input。
TEST(SpawnSubagentTool, CallsOnSpawnBeforeSendingFirstInput) {
    SubagentFixture fx;
    RecordingSessionClient recording_client;
    bool on_spawn_seen = false;
    std::string spawned_child_id;
    recording_client.on_spawn_seen = &on_spawn_seen;
    fx.deps->client = &recording_client;
    fx.deps->on_spawn = [&](const std::string& child_id,
                            const std::string& /*prompt*/) {
        spawned_child_id = child_id;
        on_spawn_seen = true;
    };

    auto r = fx.tools.execute("spawn_subagent",
                              R"({"prompt":"stage two go","wait":false})",
                              fx.ctx_for(""));
    ASSERT_TRUE(r.success) << r.output;
    ASSERT_TRUE(r.metadata.contains("subagent_session_id"));
    const std::string child_id =
        r.metadata["subagent_session_id"].get<std::string>();

    EXPECT_EQ(spawned_child_id, child_id);
    EXPECT_EQ(recording_client.sent_session_id, child_id);
    EXPECT_TRUE(recording_client.send_saw_on_spawn)
        << "tracking must be installed before the first child input is queued";
    fx.registry.destroy(child_id);
}

TEST(SpawnSubagentTool, SkillExpansionUsesInvocationAllowlist) {
    SubagentFixture fx;
    const std::string allowed = "headless-allowed-skill";
    const std::string blocked = "headless-blocked-skill";
    write_test_skill(fx.cwd, allowed, "selected by this invocation");
    write_test_skill(fx.cwd, blocked, "must remain hidden");
    fx.config.skills.allowed = std::vector<std::string>{allowed};

    RecordingSessionClient recording_client;
    fx.deps->client = &recording_client;

    const std::string allowed_prompt = "/" + allowed + " inspect this";
    auto expanded = fx.tools.execute(
        "spawn_subagent",
        std::string(R"({"prompt":")") + allowed_prompt + R"(","wait":false})",
        fx.ctx_for(""));
    ASSERT_TRUE(expanded.success) << expanded.output;
    EXPECT_EQ(recording_client.sent_display_text, allowed_prompt);
    EXPECT_NE(recording_client.sent_text, allowed_prompt);
    EXPECT_NE(recording_client.sent_text.find(allowed), std::string::npos);
    const std::string first_child =
        expanded.metadata["subagent_session_id"].get<std::string>();
    fx.registry.destroy(first_child);

    const std::string blocked_prompt = "/" + blocked + " inspect this";
    auto untouched = fx.tools.execute(
        "spawn_subagent",
        std::string(R"({"prompt":")") + blocked_prompt + R"(","wait":false})",
        fx.ctx_for(""));
    ASSERT_TRUE(untouched.success) << untouched.output;
    EXPECT_EQ(recording_client.sent_text, blocked_prompt);
    EXPECT_TRUE(recording_client.sent_display_text.empty());
    const std::string second_child =
        untouched.metadata["subagent_session_id"].get<std::string>();
    fx.registry.destroy(second_child);
}

// 场景: 子代理不能再派生子代理 —— 用子会话自己的 SessionManager 作为调用
// 上下文再次 spawn,必须被拒绝且不产生新会话。回归表现:递归派生失控。
TEST(SpawnSubagentTool, SubagentCannotSpawnFurther) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    auto first = fx.tools.execute("spawn_subagent",
                                  R"({"prompt":"child","wait":false})",
                                  fx.ctx_for(""));
    ASSERT_TRUE(first.success) << first.output;
    const std::string child_id =
        first.metadata["subagent_session_id"].get<std::string>();
    const std::size_t before = fx.registry.size();

    auto nested = fx.tools.execute("spawn_subagent",
                                   R"({"prompt":"grandchild","wait":false})",
                                   fx.ctx_for(child_id));
    EXPECT_FALSE(nested.success);
    EXPECT_NE(nested.output.find("cannot spawn"), std::string::npos);
    EXPECT_EQ(fx.registry.size(), before) << "被拒绝时不应创建新会话";
    fx.registry.destroy(child_id);
}

TEST(SpawnSubagentTool, TeamLeadCanSpawnOnlyDeclaredExpertMember) {
    SubagentFixture fx;
    acecode::ExpertDraft coordinator;
    coordinator.id = "coordinator";
    coordinator.display_name = "Coordinator";
    coordinator.profession = "Delivery";
    coordinator.lead = {
        "lead", "Coordinator", "Delivery", "Coordinate the work."};
    std::string error;
    ASSERT_TRUE(fx.experts.create_global(coordinator, &error)) << error;

    acecode::ExpertDraft tester;
    tester.id = "tester";
    tester.display_name = "Tester";
    tester.profession = "QA";
    tester.lead = {"lead", "Tester", "QA", "Test the work."};
    ASSERT_TRUE(fx.experts.create_global(tester, &error)) << error;

    acecode::ExpertDraft team;
    team.id = "delivery-team";
    team.type = acecode::ExpertType::Team;
    team.display_name = "Delivery Team";
    team.profession = "Delivery";
    team.lead_expert_id = "coordinator";
    team.member_expert_ids = {"tester"};
    ASSERT_TRUE(fx.experts.create_global(team, &error)) << error;

    acecode::SessionOptions parent_options;
    parent_options.cwd = fx.cwd.string();
    parent_options.expert_id = team.id;
    parent_options.model_name = "team-current";
    const std::string parent_id = fx.registry.create(parent_options);

    const std::size_t before = fx.registry.size();
    auto rejected = fx.tools.execute(
        "spawn_subagent",
        R"({"prompt":"attack","wait":false,"expert_member":"intruder"})",
        fx.ctx_for(parent_id));
    EXPECT_FALSE(rejected.success);
    EXPECT_NE(rejected.output.find("not selected"), std::string::npos);
    EXPECT_EQ(fx.registry.size(), before);

    RecordingSessionClient recording_client;
    fx.deps->client = &recording_client;
    auto accepted = fx.tools.execute(
        "spawn_subagent",
        R"({"prompt":"test it","wait":false,"expert_member":"tester"})",
        fx.ctx_for(parent_id));
    ASSERT_TRUE(accepted.success) << accepted.output;
    const std::string child_id =
        accepted.metadata["subagent_session_id"].get<std::string>();
    auto child = fx.registry.acquire(child_id);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->expert_id, team.id);
    EXPECT_EQ(child->expert_member_id, "tester");
    EXPECT_EQ(child->sm->current_model_preset(), "team-current");
    ASSERT_TRUE(child->expert.has_value());
    ASSERT_NE(child->expert->selected_agent("tester"), nullptr);
    EXPECT_EQ(child->expert->selected_agent("tester")->instructions, "Test the work.");

    fx.registry.destroy(child_id);
    fx.registry.destroy(parent_id);
}

TEST(SpawnSubagentTool, OrdinarySessionCannotRequestExpertMember) {
    SubagentFixture fx;
    acecode::SessionOptions parent_options;
    parent_options.cwd = fx.cwd.string();
    const std::string parent_id = fx.registry.create(parent_options);
    const auto result = fx.tools.execute(
        "spawn_subagent",
        R"({"prompt":"test it","wait":false,"expert_member":"tester"})",
        fx.ctx_for(parent_id));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("team expert lead"), std::string::npos);
    EXPECT_EQ(fx.registry.size(), 1u);
    fx.registry.destroy(parent_id);
}

// 场景: wait=true(默认)阻塞至子会话本轮结束,并把最终 assistant 答复带回
// 父上下文。EchoStreamProvider 立即完成,wait 逻辑要能在"从未观测到 busy"
// (turn 快于轮询间隔)的情况下靠新消息判定完成,而不是死等。
TEST(SpawnSubagentTool, WaitReturnsFinalAssistantReply) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    auto r = fx.tools.execute("spawn_subagent",
                              R"({"prompt":"do the work","timeout_seconds":30})",
                              fx.ctx_for(""));
    ASSERT_TRUE(r.success) << r.output;
    EXPECT_NE(r.output.find("subagent-final-reply"), std::string::npos)
        << "父上下文应拿到子会话的最终答复,实际: " << r.output;
    ASSERT_TRUE(r.metadata.contains("subagent_session_id"));
    fx.registry.destroy(r.metadata["subagent_session_id"].get<std::string>());
}

// 场景: 有父会话上下文的 spawn → 子会话 entry 与持久化 meta 都记录
// parent_session_id(「后台任务」面板/列表过滤的数据源)。用 wait=true
// 保证子会话 turn 已结束、meta 已落盘。一旦回归:子会话泄漏进侧栏列表。
TEST(SpawnSubagentTool, ChildRecordsParentSessionId) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    acecode::SessionOptions parent_opts;
    parent_opts.cwd = fx.cwd.string();
    const std::string parent_id = fx.registry.create(parent_opts);
    ASSERT_FALSE(parent_id.empty());

    auto r = fx.tools.execute("spawn_subagent",
                              R"({"prompt":"go","timeout_seconds":30})",
                              fx.ctx_for(parent_id));
    ASSERT_TRUE(r.success) << r.output;
    const std::string child_id =
        r.metadata["subagent_session_id"].get<std::string>();

    auto child = fx.registry.acquire(child_id);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->parent_session_id, parent_id);
    ASSERT_NE(child->sm, nullptr);
    auto meta = child->sm->load_session_meta(child_id);
    ASSERT_EQ(meta.id, child_id) << "子会话 meta 应已落盘";
    EXPECT_EQ(meta.parent_session_id, parent_id)
        << "parent_session_id 必须持久化,否则 daemon 重启后子会话泄漏进侧栏";

    fx.registry.destroy(child_id);
    fx.registry.destroy(parent_id);
}

// 场景: daemon 重启(registry 冷启动)后 resume 一个带 parent_session_id
// 的子会话 → 子会话身份(parent_session_id + subagent_depth=1)从 meta 恢复,
// 深度限制继续生效。一旦回归:重启后的子会话能再派生孙代理。
TEST(SpawnSubagentTool, ResumeRestoresSubagentIdentityFromMeta) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    const std::string child_id = "20260705-010203-beef";
    const auto project_dir =
        acecode::SessionStorage::get_project_dir(fx.cwd.string());
    fs::create_directories(project_dir);
    {
        // 手造持久化数据:meta 带 parent_session_id,jsonl 只需存在。
        acecode::SessionMeta meta;
        meta.id = child_id;
        meta.cwd = fx.cwd.string();
        meta.parent_session_id = "20260705-010000-cafe";
        acecode::SessionStorage::write_meta(
            acecode::SessionStorage::meta_path(project_dir, child_id), meta);
        std::ofstream jsonl(
            acecode::SessionStorage::session_path(project_dir, child_id));
        jsonl << "";
    }

    acecode::SessionOptions opts;
    opts.cwd = fx.cwd.string();
    ASSERT_TRUE(fx.registry.resume(child_id, opts));
    auto entry = fx.registry.acquire(child_id);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->parent_session_id, "20260705-010000-cafe");
    EXPECT_EQ(entry->subagent_depth, 1)
        << "深度限制必须随 meta 恢复,防重启后递归派生";

    auto nested = fx.tools.execute("spawn_subagent",
                                   R"({"prompt":"grandchild","wait":false})",
                                   fx.ctx_for(child_id));
    EXPECT_FALSE(nested.success);
    fx.registry.destroy(child_id);
}

// 场景: wait_subagent 对不存在的 session id → 明确报错,不阻塞。
TEST(WaitSubagentTool, UnknownSessionRejected) {
    SubagentFixture fx;
    auto r = fx.tools.execute("wait_subagent",
                              R"({"session_id":"nope-123"})",
                              fx.ctx_for(""));
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("unknown session"), std::string::npos);
}

// 场景: wait_subagent 对已完成的子会话 → 直接取回其最新答复(配合
// spawn(wait=false) 的 fan-out / join 用法)。
TEST(WaitSubagentTool, CollectsReplyFromFinishedSubagent) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    auto spawned = fx.tools.execute("spawn_subagent",
                                    R"({"prompt":"go","wait":false})",
                                    fx.ctx_for(""));
    ASSERT_TRUE(spawned.success) << spawned.output;
    const std::string child_id =
        spawned.metadata["subagent_session_id"].get<std::string>();

    auto r = fx.tools.execute(
        "wait_subagent",
        std::string(R"({"session_id":")") + child_id + R"(","timeout_seconds":30})",
        fx.ctx_for(""));
    ASSERT_TRUE(r.success) << r.output;
    EXPECT_NE(r.output.find("subagent-final-reply"), std::string::npos);
    fx.registry.destroy(child_id);
}

namespace {

// 只会报错的 stub:模拟 provider 终止错误(断连 / 网关 5xx)。子会话的回合
// 因此没有正常的最终答复。
class FailingStreamProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>&,
        const std::vector<acecode::ToolDef>&) override {
        acecode::ChatResponse resp;
        resp.finish_reason = "stop";
        return resp;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* = nullptr) override {
        acecode::StreamEvent error;
        error.type = acecode::StreamEventType::Error;
        error.error = "connection reset by stub";
        error.provider_error.kind = acecode::ProviderErrorKind::Network;
        error.provider_error.display_message = "connection reset by stub";
        callback(error);
    }

    std::string name() const override { return "failing-stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "failing-stub"; }
    void set_model(const std::string&) override {}
};

} // namespace

// 场景: 父会话已进入 worktree(EnterWorktree / Web pill / LOOP 同一形态:
// SessionManager 记着 WorktreeSessionInfo,AgentLoop cwd 已切到 worktree),
// 此时 spawn 子代理。
// 期望: 子会话共享同一个 worktree —— meta 里的 worktree 标 inherited、AgentLoop
// cwd 是 worktree 路径、write_root 是 worktree 路径;而 entry->cwd 是父会话
// 进 worktree 前的 workspace cwd,子会话与父会话落在同一个 project dir。
// 回归表现(修复前): 子会话只拿到 worktree 路径当普通 cwd,系统提示说
// "Session worktree: inactive",Yolo 下没有任何写边界,能把改动写进主 checkout;
// 且 meta 落在 worktree 路径的 project dir 下,后台任务面板在它结束后找不到。
TEST(SpawnSubagentTool, ChildSharesParentWorktreeWithWriteBoundary) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    acecode::SessionOptions parent_opts;
    parent_opts.cwd = fx.cwd.string();
    const std::string parent_id = fx.registry.create(parent_opts);
    auto parent = fx.registry.acquire(parent_id);
    ASSERT_NE(parent, nullptr);

    const fs::path worktree_dir = fx.cwd / ".acecode" / "worktrees" / "feat";
    fs::create_directories(worktree_dir);
    acecode::WorktreeSessionInfo worktree;
    worktree.original_cwd = fx.cwd.string();
    worktree.worktree_path = worktree_dir.string();
    worktree.worktree_name = "feat";
    worktree.worktree_branch = "worktree-feat";
    parent->sm->set_active_worktree(worktree);
    parent->loop->set_cwd(worktree.worktree_path);
    ASSERT_EQ(parent->loop->write_root(), worktree.worktree_path);

    auto ctx = fx.ctx_for(parent_id);
    ctx.cwd = worktree.worktree_path;
    ctx.write_root = parent->loop->write_root();
    auto r = fx.tools.execute("spawn_subagent",
                              R"({"prompt":"fix module a","wait":false})", ctx);
    ASSERT_TRUE(r.success) << r.output;
    const std::string child_id = r.metadata["subagent_session_id"].get<std::string>();
    auto child = fx.registry.acquire(child_id);
    ASSERT_NE(child, nullptr);

    EXPECT_EQ(child->cwd, fx.cwd.string()) << "子会话应与父会话同 workspace / project dir";
    EXPECT_EQ(child->loop->cwd(), worktree.worktree_path);
    EXPECT_EQ(child->loop->write_root(), worktree.worktree_path);
    const auto inherited = child->sm->active_worktree();
    EXPECT_TRUE(inherited.active());
    EXPECT_TRUE(inherited.inherited);
    EXPECT_EQ(inherited.worktree_path, worktree.worktree_path);
    EXPECT_EQ(inherited.worktree_branch, "worktree-feat");
    // fx.cwd 不是 git 仓库:读不到主 checkout 快照就不监视,而不是拿空基线乱报。
    EXPECT_TRUE(child->workspace_watch_cwd.empty());

    fx.registry.destroy(child_id);
    fx.registry.destroy(parent_id);
}

// 场景: 父会话是 daemon LOOP 运行(loop_execution + <loop-execution> 系统上下文,
// 无 worktree),spawn 子代理。
// 期望: 子会话继承 LOOP 身份与执行策略 —— entry->loop_execution / loop_id /
// loop_run_id 与父会话一致,AgentLoop 的策略 active 且 system_context 相同,
// 写边界 = 子会话 cwd(LOOP 语义)。
// 回归表现(修复前): 子会话不是 LOOP 会话,LOOP 主会话那道写边界与 shell 写
// 守卫对它统统不生效,LOOP 的隔离只罩住主会话。
TEST(SpawnSubagentTool, ChildOfLoopRunInheritsExecutionPolicy) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    acecode::SessionOptions parent_opts;
    parent_opts.cwd = fx.cwd.string();
    parent_opts.loop_execution = true;
    parent_opts.loop_id = "loop-1";
    parent_opts.loop_run_id = "run-1";
    parent_opts.loop_system_context = "You are executing daemon-owned LOOP 'nightly'.";
    const std::string parent_id = fx.registry.create(parent_opts);

    auto r = fx.tools.execute("spawn_subagent",
                              R"({"prompt":"stage one","wait":false})",
                              fx.ctx_for(parent_id));
    ASSERT_TRUE(r.success) << r.output;
    const std::string child_id = r.metadata["subagent_session_id"].get<std::string>();
    auto child = fx.registry.acquire(child_id);
    ASSERT_NE(child, nullptr);

    EXPECT_TRUE(child->loop_execution);
    EXPECT_EQ(child->loop_id, "loop-1");
    EXPECT_EQ(child->loop_run_id, "run-1");
    EXPECT_TRUE(child->loop->loop_execution_policy().active);
    EXPECT_EQ(child->loop->loop_execution_policy().system_context,
              "You are executing daemon-owned LOOP 'nightly'.");
    EXPECT_EQ(child->loop->write_root(), child->loop->cwd());

    fx.registry.destroy(child_id);
    fx.registry.destroy(parent_id);
}

// 场景: 子会话的回合因 provider 终止错误结束(断连 / 上下文超限),没有正常的
// 最终答复。
// 期望: spawn(wait=true) 的工具结果 success=false,文案说明 "failed before
// finishing" 并带错误原因;父代理据此重派或上报,而不是把半截叙述当成果。
// 回归表现(修复前): wait 报 "[subagent ... completed]" + 子会话临终前的一句
// 过程叙述,父会话以为任务做完,直到检查产物才发现是空的。
TEST(SpawnSubagentTool, WaitReportsChildTurnFailureInsteadOfCompleted) {
    SubagentFixture fx;
    fx.provider = std::make_shared<FailingStreamProvider>();

    auto r = fx.tools.execute("spawn_subagent",
                              R"({"prompt":"do the work","timeout_seconds":30})",
                              fx.ctx_for(""));
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("failed before finishing"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("connection reset by stub"), std::string::npos) << r.output;
    EXPECT_EQ(r.output.find("completed]"), std::string::npos) << r.output;
    ASSERT_TRUE(r.metadata.contains("subagent_session_id"));
    fx.registry.destroy(r.metadata["subagent_session_id"].get<std::string>());
}

// 场景: 父会话在 worktree 里,spawn(wait=false) 之后有人在主 checkout 里新建了
// 文件(模拟子代理经 bash 脚本绕过工具层边界),再 wait_subagent。
// 期望: 等待结果附 "WARNING: the main checkout ... changed" 与该文件路径,
// metadata.workspace_touched 列出它;父代理当场知道有写入落到了 worktree 之外。
// 再等一次不重复报告(基线前移)。
TEST(SpawnSubagentTool, WaitReportsMainCheckoutChangesMadeWhileChildRan) {
    if (!acecode::worktree::run_git({"--version"}, "").ok()) {
        GTEST_SKIP() << "git not available on this machine";
    }
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();
    const std::string repo = acecode::path_to_utf8(fx.cwd);
    ASSERT_TRUE(acecode::worktree::run_git({"init", "-b", "main"}, repo).ok());

    acecode::SessionOptions parent_opts;
    parent_opts.cwd = repo;
    const std::string parent_id = fx.registry.create(parent_opts);
    auto parent = fx.registry.acquire(parent_id);
    ASSERT_NE(parent, nullptr);
    // 假 worktree 放在仓库外面:子会话的临时文件不能混进主 checkout 的 status。
    const fs::path worktree_dir = fx.cwd.parent_path() / (fx.cwd.filename().string() + "_wt");
    fs::create_directories(worktree_dir);
    acecode::WorktreeSessionInfo worktree;
    worktree.original_cwd = repo;
    worktree.worktree_path = acecode::path_to_utf8(worktree_dir);
    worktree.worktree_name = "feat";
    worktree.worktree_branch = "worktree-feat";
    parent->sm->set_active_worktree(worktree);
    parent->loop->set_cwd(worktree.worktree_path);

    auto ctx = fx.ctx_for(parent_id);
    ctx.cwd = worktree.worktree_path;
    ctx.write_root = worktree.worktree_path;
    auto spawned = fx.tools.execute("spawn_subagent",
                                    R"({"prompt":"go","wait":false})", ctx);
    ASSERT_TRUE(spawned.success) << spawned.output;
    const std::string child_id = spawned.metadata["subagent_session_id"].get<std::string>();
    {
        auto child = fx.registry.acquire(child_id);
        ASSERT_NE(child, nullptr);
        EXPECT_EQ(child->workspace_watch_cwd, repo);
    }
    {
        std::ofstream out(fx.cwd / "escaped.txt", std::ios::binary);
        out << "written outside the worktree\n";
    }

    const std::string wait_args =
        std::string(R"({"session_id":")") + child_id + R"(","timeout_seconds":30})";
    auto waited = fx.tools.execute("wait_subagent", wait_args, ctx);
    EXPECT_NE(waited.output.find("WARNING: the main checkout"), std::string::npos)
        << waited.output;
    EXPECT_NE(waited.output.find("escaped.txt"), std::string::npos) << waited.output;
    ASSERT_TRUE(waited.metadata.contains("workspace_touched"));
    EXPECT_EQ(waited.metadata["workspace_touched"].size(), 1u);

    auto again = fx.tools.execute("wait_subagent", wait_args, ctx);
    EXPECT_EQ(again.output.find("WARNING: the main checkout"), std::string::npos)
        << again.output;

    fx.registry.destroy(child_id);
    fx.registry.destroy(parent_id);
    std::error_code ec;
    fs::remove_all(worktree_dir, ec);
}
