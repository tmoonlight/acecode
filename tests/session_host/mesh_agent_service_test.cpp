// 覆盖 src/host/session_host/mesh/ 与 tools/mesh_agent_tools.cpp(add-mesh-swarm-mode):
// 蜂群模式（网状）= Codex Multi-Agent V2 的复刻。这里用真实 SessionRegistry + 脚本化
// provider 跑完整的 agent 树:spawn → 子 agent 自动开工 → FINAL_ANSWER 回报父 agent;
// send_message 只排队、followup_task 唤醒;并发上限下的 LRU 换出与按需恢复;
// interrupt 不回报;退出网状模式的守卫;重启后按索引重建树。
//
// provider 规则(按最后一条 user 消息的 Payload 判定):含 BLOCK 时阻塞到 release()
// 或回合被中止;含 FAIL 时返回不可重试的 400;否则回复 "answer:<payload>"。

#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "config/config.hpp"
#include "permissions/permissions.hpp"
#include "session/inter_agent_message.hpp"
#include "session/mesh_tree_index.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session_host/mesh/mesh_agent_service.hpp"
#include "session_host/session_registry.hpp"
#include "session_host/tools/mesh_agent_tools.hpp"
#include "tool/tool_executor.hpp"
#include "utils/paths.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using acecode::mesh::AgentStatus;
using acecode::mesh::MeshAgentService;

namespace {

class MeshScriptProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>&,
                               const std::vector<acecode::ToolDef>&) override {
        acecode::ChatResponse response;
        response.content = "unused";
        response.finish_reason = "stop";
        return response;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>& messages,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* abort_flag = nullptr) override {
        std::string payload;
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role != "user") continue;
            payload = acecode::mesh::inter_agent_payload_from_content(it->content);
            if (payload.empty()) payload = it->content;
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            requests_.push_back(messages);
            payloads_.push_back(payload);
        }
        cv_.notify_all();
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        if (payload.find("BLOCK") != std::string::npos) {
            std::unique_lock<std::mutex> lock(mu_);
            while (!released_ && !(abort_flag && abort_flag->load())) {
                cv_.wait_for(lock, 5ms);
            }
            if (abort_flag && abort_flag->load()) {
                lock.unlock();
                callback(done);
                return;
            }
        }
        if (payload.find("FAIL") != std::string::npos) {
            acecode::StreamEvent error;
            error.type = acecode::StreamEventType::Error;
            error.provider_error.kind = acecode::ProviderErrorKind::Http;
            error.provider_error.status_code = 400;
            error.provider_error.display_message = "bad request boom";
            error.error = "bad request boom";
            callback(error);
            return;
        }
        acecode::StreamEvent delta;
        delta.type = acecode::StreamEventType::Delta;
        delta.content = "answer:" + payload;
        callback(delta);
        callback(done);
    }

    std::string name() const override { return "mesh-stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "mesh-stub-1"; }
    void set_model(const std::string&) override {}

    void release() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            released_ = true;
        }
        cv_.notify_all();
    }
    int request_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return static_cast<int>(payloads_.size());
    }
    std::vector<std::string> payloads() const {
        std::lock_guard<std::mutex> lock(mu_);
        return payloads_;
    }
    std::vector<acecode::ChatMessage> request(int index) const {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_.at(static_cast<std::size_t>(index));
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::vector<acecode::ChatMessage>> requests_;
    std::vector<std::string> payloads_;
    bool released_ = false;
};

bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

bool any_content_contains(const std::vector<acecode::ChatMessage>& messages,
                          const std::string& needle) {
    for (const auto& message : messages) {
        if (message.content.find(needle) != std::string::npos) return true;
    }
    return false;
}

const char* home_key() {
#ifdef _WIN32
    return "USERPROFILE";
#else
    return "HOME";
#endif
}

void set_env(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

class MeshAgentServiceTest : public testing::Test {
protected:
    fs::path home;
    std::string workspace;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AppConfig config;
    std::shared_ptr<MeshScriptProvider> provider = std::make_shared<MeshScriptProvider>();
    std::unique_ptr<acecode::SessionRegistry> registry;
    std::shared_ptr<MeshAgentService> service;
    std::string root_id;

    void SetUp() override {
        home = fs::temp_directory_path() /
            ("acecode_mesh_service_" + std::to_string(std::random_device{}()));
        fs::create_directories(home / "ws");
        workspace = (home / "ws").string();
        if (const char* previous = std::getenv(home_key())) {
            previous_home_ = previous;
            had_home_ = true;
        }
        set_env(home_key(), home.string());
        previous_mode_ = acecode::override_run_mode_for_test(acecode::RunMode::User);
        acecode::reset_data_dir_cache_for_test();

        acecode::SessionRegistryDeps deps;
        deps.provider_accessor = [p = provider] { return std::shared_ptr<acecode::LlmProvider>(p); };
        deps.tools = &tools;
        deps.cwd = workspace;
        deps.template_permissions = &permissions;
        deps.no_workspace_cache_root = (home / "no-workspace").string();
        deps.auto_title_generator = [](const std::string&) {
            return std::optional<std::string>{"title"};
        };
        registry = std::make_unique<acecode::SessionRegistry>(std::move(deps));
    }

    void TearDown() override {
        provider->release();
        if (service) service->shutdown();
        service.reset();
        if (registry) registry->shutdown_all();
        registry.reset();
        if (had_home_) {
            set_env(home_key(), previous_home_);
        } else {
            set_env(home_key(), "");
        }
        acecode::override_run_mode_for_test(previous_mode_);
        acecode::reset_data_dir_cache_for_test();
        std::error_code ec;
        fs::remove_all(home, ec);
    }

    // 配置在服务启动前定好(工具描述与上限都在注册 / 调用时读取)。
    void start_service() {
        MeshAgentService::Deps deps;
        deps.registry = registry.get();
        deps.config = &config;
        service = std::make_shared<MeshAgentService>(std::move(deps));
        service->attach();
    }

    std::string create_root(const std::string& swarm_mode = "mesh") {
        acecode::SessionOptions opts;
        opts.cwd = workspace;
        opts.swarm_mode = swarm_mode;
        const std::string id = registry->create(opts);
        if (swarm_mode == "mesh") root_id = id;
        return id;
    }

    acecode::ToolContext ctx_for(const std::string& session_id) {
        acecode::ToolContext ctx;
        ctx.cwd = workspace;
        ctx.session_id = session_id;
        auto entry = registry->acquire(session_id);
        if (entry) {
            ctx.session_manager = entry->sm.get();
            kept_.push_back(entry);
        }
        return ctx;
    }

    acecode::AgentLoop* loop(const std::string& session_id) {
        auto entry = registry->acquire(session_id);
        if (!entry) return nullptr;
        kept_.push_back(entry);
        return entry->loop.get();
    }

    std::size_t root_mail() {
        auto* root = loop(root_id);
        return root ? root->pending_mailbox_count() : 0;
    }

    acecode::mesh::SpawnResult spawn(const std::string& caller, const std::string& task,
                                     const std::string& message) {
        acecode::mesh::SpawnArgs args;
        args.task_name = task;
        args.message = message;
        return service->spawn(ctx_for(caller), args);
    }

    std::vector<acecode::mesh::ListedAgent> list(const std::string& caller,
                                                 const std::string& prefix = {}) {
        std::vector<acecode::mesh::ListedAgent> agents;
        EXPECT_EQ(service->list(ctx_for(caller), prefix, agents), "");
        return agents;
    }

    static const acecode::mesh::ListedAgent* find(
        const std::vector<acecode::mesh::ListedAgent>& agents, const std::string& path) {
        for (const auto& agent : agents) {
            if (agent.path == path) return &agent;
        }
        return nullptr;
    }

private:
    std::vector<std::shared_ptr<acecode::SessionEntry>> kept_;
    std::string previous_home_;
    bool had_home_ = false;
    acecode::RunMode previous_mode_ = acecode::RunMode::User;
};

} // namespace

// 场景:根 agent agent_spawn 一个子 agent。
// 期望:子会话挂在根下(parent_session_id = 根 id,路径 /root/worker,模式 mesh,标题 = 任务名),
// 收到 NEW_TASK 信封后自动开工;结束后 FINAL_ANSWER 进根 agent 邮箱但**不唤醒**空闲的根
// (Codex:子 agent 完成不唤醒父 agent);根的下一个回合能看到这份最终回答;树索引已落盘。
TEST_F(MeshAgentServiceTest, SpawnRunsChildAndRoutesFinalAnswerToIdleRoot) {
    start_service();
    create_root();
    const auto spawned = spawn(root_id, "worker", "write the report");
    ASSERT_EQ(spawned.error, "");
    EXPECT_EQ(spawned.path, "/root/worker");
    ASSERT_FALSE(spawned.session_id.empty());

    ASSERT_TRUE(wait_until([&] { return root_mail() == 1; }));
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(provider->request_count(), 1);
    EXPECT_FALSE(loop(root_id)->is_busy());

    const auto first = provider->request(0);
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first.back().content,
              "<inter_agent_message>\nMessage Type: NEW_TASK\nTask name: /root/worker\n"
              "Sender: /root\nPayload:\nwrite the report\n</inter_agent_message>");

    auto child = registry->acquire(spawned.session_id);
    ASSERT_TRUE(child);
    EXPECT_EQ(child->sm->current_parent_session_id(), root_id);
    EXPECT_EQ(child->sm->current_swarm_mode(), "mesh");
    EXPECT_EQ(child->sm->current_agent_path(), "/root/worker");

    const auto agents = list(root_id);
    ASSERT_EQ(agents.size(), 2u);
    EXPECT_EQ(agents[0].path, "/root");
    const auto* worker = find(agents, "/root/worker");
    ASSERT_NE(worker, nullptr);
    EXPECT_EQ(acecode::mesh::agent_status_to_json(worker->status),
              (nlohmann::json{{"completed", "answer:write the report"}}));

    const auto index = acecode::mesh::read_mesh_tree_index(
        child->sm->current_project_dir(), root_id);
    ASSERT_EQ(index.size(), 1u);
    EXPECT_EQ(index[0].session_id, spawned.session_id);

    // 根的下一个回合:用户消息之后并入子 agent 的 FINAL_ANSWER。
    loop(root_id)->submit("how did it go?");
    ASSERT_TRUE(wait_until([&] { return provider->request_count() == 2; }));
    EXPECT_TRUE(any_content_contains(provider->request(1),
        "Message Type: FINAL_ANSWER\nTask name: /root\nSender: /root/worker\nPayload:\n"
        "answer:write the report"));
    ASSERT_TRUE(wait_until([&] { return !loop(root_id)->is_busy(); }));
}

// 场景:对已完成的子 agent 先 agent_send_message 再 agent_followup_task。
// 期望:send_message 只排队不开回合;followup_task 唤醒它,唤醒回合同时带上之前排队的
// 消息(先排队的在前),完成后第二份 FINAL_ANSWER 进根邮箱;各类错误文案对齐 Codex。
TEST_F(MeshAgentServiceTest, FollowupWakesIdleChildWhileSendMessageOnlyQueues) {
    start_service();
    create_root();
    const auto spawned = spawn(root_id, "worker", "first task");
    ASSERT_EQ(spawned.error, "");
    ASSERT_TRUE(wait_until([&] { return root_mail() == 1; }));

    const auto root_ctx = ctx_for(root_id);
    EXPECT_EQ(service->deliver(root_ctx, "worker", "fyi only", false), "");
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(provider->request_count(), 1);
    EXPECT_EQ(loop(spawned.session_id)->pending_mailbox_count(), 1u);

    EXPECT_EQ(service->deliver(root_ctx, "/root/worker", "second task", true), "");
    ASSERT_TRUE(wait_until([&] { return root_mail() == 2; }));
    ASSERT_EQ(provider->request_count(), 2);
    const auto woken = provider->request(1);
    EXPECT_TRUE(any_content_contains(woken, "Message Type: MESSAGE\nTask name: /root/worker\n"
                                            "Sender: /root\nPayload:\nfyi only"));
    EXPECT_EQ(provider->payloads()[1], "second task");

    EXPECT_EQ(service->deliver(root_ctx, "/root", "x", true),
              "Follow-up tasks can't target the root agent");
    EXPECT_EQ(service->deliver(root_ctx, "nobody", "x", false),
              "live agent path `/root/nobody` not found");
    EXPECT_EQ(service->deliver(root_ctx, "worker", "  \n", false),
              "Empty message can't be sent to an agent");
}

// 场景:agent_spawn 的参数与调用方校验。
// 期望:非网状会话调用被拒;非法任务名、未知模型、无团队专家时的 agent_type、重名路径、
// 空消息都返回 Codex 同款(或对应改写的)错误,且不创建任何子会话。
TEST_F(MeshAgentServiceTest, SpawnValidatesCallerAndArguments) {
    acecode::ModelProfile fast;
    fast.name = "fast";
    fast.provider = "openai";
    fast.model = "gpt-fast";
    config.saved_models.push_back(fast);
    start_service();
    const std::string plain = create_root("off");
    EXPECT_EQ(spawn(plain, "worker", "go").error,
              "agent collaboration tools are only available in mesh swarm mode.");
    create_root();
    EXPECT_EQ(spawn(root_id, "Bad-Name", "go").error,
              "agent_name must use only lowercase letters, digits, and underscores");
    EXPECT_EQ(spawn(root_id, "worker", " ").error, "Empty message can't be sent to an agent");

    acecode::mesh::SpawnArgs args;
    args.task_name = "worker";
    args.message = "go";
    args.model = "missing";
    EXPECT_EQ(service->spawn(ctx_for(root_id), args).error,
              "Unknown model `missing` for agent_spawn. Available models: fast");
    args.model.clear();
    args.agent_type = "reviewer";
    EXPECT_EQ(service->spawn(ctx_for(root_id), args).error,
              "agent_type is only available when this session is bound to a team expert");
    const std::size_t sessions_before = registry->size();

    ASSERT_EQ(spawn(root_id, "worker", "go").error, "");
    EXPECT_EQ(spawn(root_id, "worker", "again").error, "agent path `/root/worker` already exists");
    EXPECT_EQ(registry->size(), sessions_before + 1);
}

// 场景:并发上限 3(根占 1 个名额,子 agent 最多驻留 2 个),依次派出 a、b、c。
// 期望:派 c 时换出最久没有活动且已完成的 a(Codex V2Residency 的 LRU);a 不再出现在
// agent_list 里,但树里仍有记录;对 a 发 followup_task 会从磁盘恢复它(这次换出 b),
// 恢复后的 a 带着原来的上下文处理新任务。
TEST_F(MeshAgentServiceTest, FullTreeEvictsLeastRecentlyActiveAgentAndRestoresOnDemand) {
    config.swarm.mesh.max_concurrent_agents = 3;
    start_service();
    create_root();
    const auto a = spawn(root_id, "a", "task a");
    ASSERT_EQ(a.error, "");
    ASSERT_TRUE(wait_until([&] { return root_mail() == 1; }));
    const auto b = spawn(root_id, "b", "task b");
    ASSERT_EQ(b.error, "");
    ASSERT_TRUE(wait_until([&] { return root_mail() == 2; }));
    const auto c = spawn(root_id, "c", "task c");
    ASSERT_EQ(c.error, "");
    ASSERT_TRUE(wait_until([&] { return root_mail() == 3; }));

    EXPECT_EQ(registry->acquire(a.session_id), nullptr);
    const auto listed = list(root_id);
    EXPECT_EQ(find(listed, "/root/a"), nullptr);
    EXPECT_NE(find(listed, "/root/b"), nullptr);
    EXPECT_NE(find(listed, "/root/c"), nullptr);
    bool a_known_unloaded = false;
    for (const auto& info : service->tree_agents(root_id)) {
        if (info.path == "/root/a") a_known_unloaded = !info.loaded;
    }
    EXPECT_TRUE(a_known_unloaded);

    EXPECT_EQ(service->deliver(ctx_for(root_id), "a", "task a2", true), "");
    ASSERT_TRUE(wait_until([&] { return root_mail() == 4; }));
    EXPECT_NE(registry->acquire(a.session_id), nullptr);
    EXPECT_EQ(registry->acquire(b.session_id), nullptr);
    const auto restored = provider->request(provider->request_count() - 1);
    EXPECT_EQ(provider->payloads().back(), "task a2");
    EXPECT_TRUE(any_content_contains(restored, "answer:task a"));
}

// 场景:并发上限 2(只能驻留 1 个子 agent),而这个子 agent 正在运行。
// 期望:再派第二个返回 "collab spawn failed: agent thread limit reached",不会换出运行中的
// agent;运行中的 agent 结束后名额可以复用。
TEST_F(MeshAgentServiceTest, ThreadLimitReachedWhenNoAgentCanBeEvicted) {
    config.swarm.mesh.max_concurrent_agents = 2;
    start_service();
    create_root();
    const auto busy = spawn(root_id, "busy", "BLOCK until released");
    ASSERT_EQ(busy.error, "");
    ASSERT_TRUE(wait_until([&] { return provider->request_count() == 1; }));
    EXPECT_EQ(spawn(root_id, "other", "go").error,
              "collab spawn failed: agent thread limit reached");
    provider->release();
    ASSERT_TRUE(wait_until([&] { return root_mail() == 1; }));
    EXPECT_EQ(spawn(root_id, "other", "go").error, "");
}

// 场景:agent_interrupt 打断运行中的子 agent;根与自身不可被打断。
// 期望:返回打断前状态 running;被打断的回合不发 FINAL_ANSWER(Codex:interrupted
// 不回报),之后状态是 interrupted;打断根报 "root is not a spawned agent",
// 打断自己报 Codex 原文。
TEST_F(MeshAgentServiceTest, InterruptAbortsRunningChildWithoutFinalAnswer) {
    start_service();
    create_root();
    const auto slow = spawn(root_id, "slow", "BLOCK please");
    ASSERT_EQ(slow.error, "");
    ASSERT_TRUE(wait_until([&] { return provider->request_count() == 1; }));
    ASSERT_TRUE(wait_until([&] { return loop(slow.session_id)->is_busy(); }));

    AgentStatus previous;
    EXPECT_EQ(service->interrupt(ctx_for(root_id), "slow", previous), "");
    EXPECT_EQ(previous.kind, AgentStatus::Kind::Running);
    ASSERT_TRUE(wait_until([&] { return !loop(slow.session_id)->is_busy(); }));
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(root_mail(), 0u);
    const auto agents = list(root_id);
    const auto* listed = find(agents, "/root/slow");
    ASSERT_NE(listed, nullptr);
    EXPECT_EQ(acecode::mesh::agent_status_to_json(listed->status), "interrupted");

    EXPECT_EQ(service->interrupt(ctx_for(root_id), "/root", previous),
              "root is not a spawned agent");
    EXPECT_EQ(service->interrupt(ctx_for(slow.session_id), "/root/slow", previous),
              "an agent cannot interrupt itself; return your result and let the parent "
              "interrupt you if needed");
}

// 场景:子 agent 的回合因 provider 400 失败。
// 期望:父 agent 收到 FINAL_ANSWER(状态 errored),agent_list 显示 {"errored": ...};
// 回报正文是 Codex 的 "Agent errored: ..." + 下一步提示。
TEST_F(MeshAgentServiceTest, ErroredChildReportsAgentErroredToParent) {
    start_service();
    create_root();
    const auto broken = spawn(root_id, "broken", "FAIL now");
    ASSERT_EQ(broken.error, "");
    ASSERT_TRUE(wait_until([&] { return root_mail() == 1; }));
    const auto agents = list(root_id);
    const auto* listed = find(agents, "/root/broken");
    ASSERT_NE(listed, nullptr);
    EXPECT_EQ(listed->status.kind, AgentStatus::Kind::Errored);
    EXPECT_FALSE(listed->status.errored.empty());

    loop(root_id)->submit("status?");
    ASSERT_TRUE(wait_until([&] { return provider->request_count() == 2; }));
    EXPECT_TRUE(any_content_contains(provider->request(1),
        "Sender: /root/broken\nPayload:\nAgent errored: "));
    EXPECT_TRUE(any_content_contains(provider->request(1),
        "This agent's turn failed. If you still need this agent, use the available "
        "collaboration tools to give it another task."));
    ASSERT_TRUE(wait_until([&] { return !loop(root_id)->is_busy(); }));
}

// 场景:子 agent /root/lead 再派孙 agent /root/lead/sub(深度不限)。
// 期望:孙会话的 parent_session_id 仍是根 id(扁平挂在根下,复用后台任务面板);
// 孙的 FINAL_ANSWER 只进 lead 的邮箱,不进根;lead 视角的相对 path_prefix 生效。
TEST_F(MeshAgentServiceTest, NestedChildReportsToItsParentNotRoot) {
    start_service();
    create_root();
    const auto lead = spawn(root_id, "lead", "lead task");
    ASSERT_EQ(lead.error, "");
    ASSERT_TRUE(wait_until([&] { return root_mail() == 1; }));

    const auto sub = spawn(lead.session_id, "sub", "sub task");
    ASSERT_EQ(sub.error, "");
    EXPECT_EQ(sub.path, "/root/lead/sub");
    ASSERT_TRUE(wait_until([&] { return loop(lead.session_id)->pending_mailbox_count() == 1; }));
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(root_mail(), 1u);
    EXPECT_EQ(registry->acquire(sub.session_id)->sm->current_parent_session_id(), root_id);

    const auto scoped = list(lead.session_id, "sub");
    ASSERT_EQ(scoped.size(), 1u);
    EXPECT_EQ(scoped[0].path, "/root/lead/sub");
}

// 场景:根会话在子 agent 运行时尝试退出网状模式;子 agent 自己尝试改模式。
// 期望:前者被守卫拒绝(否则运行中的 agent 失去回报对象),子 agent 结束后允许;
// 后者一律拒绝 —— 网状子 agent 的身份由路径决定,不能切成别的模式。
TEST_F(MeshAgentServiceTest, LeavingMeshIsRefusedWhileAgentsRun) {
    start_service();
    create_root();
    const auto busy = spawn(root_id, "busy", "BLOCK work");
    ASSERT_EQ(busy.error, "");
    ASSERT_TRUE(wait_until([&] { return provider->request_count() == 1; }));

    std::string error;
    EXPECT_FALSE(registry->set_swarm_mode(root_id, acecode::SwarmMode::Off, &error));
    EXPECT_NE(error.find("is still running"), std::string::npos) << error;
    EXPECT_FALSE(registry->set_swarm_mode(busy.session_id, acecode::SwarmMode::Star, &error));
    EXPECT_EQ(error, "swarm mode cannot be changed for a mesh swarm sub-agent");

    provider->release();
    ASSERT_TRUE(wait_until([&] { return root_mail() == 1; }));
    ASSERT_TRUE(wait_until([&] { return !loop(busy.session_id)->is_busy(); }));
    EXPECT_TRUE(registry->set_swarm_mode(root_id, acecode::SwarmMode::Off, &error)) << error;
    EXPECT_EQ(registry->swarm_mode(root_id), acecode::SwarmMode::Off);
}

// 场景:模拟 daemon 重启 —— 子会话已卸载、服务实例重建。
// 期望:新服务按根会话目录里的索引重建树,子 agent 显示为未加载;对它发 followup_task
// 会经 SessionRegistry::resume 恢复并继续工作。
TEST_F(MeshAgentServiceTest, RestartRebuildsTreeFromIndexAndRestoresOnDemand) {
    start_service();
    create_root();
    const auto worker = spawn(root_id, "worker", "first");
    ASSERT_EQ(worker.error, "");
    ASSERT_TRUE(wait_until([&] { return root_mail() == 1; }));
    ASSERT_TRUE(wait_until([&] { return !loop(worker.session_id)->is_busy(); }));

    service->shutdown();
    service.reset();
    registry->destroy(worker.session_id);
    start_service();

    const auto agents = list(root_id);
    EXPECT_EQ(find(agents, "/root/worker"), nullptr);
    bool known = false;
    for (const auto& info : service->tree_agents(root_id)) {
        if (info.path == "/root/worker") known = !info.loaded;
    }
    EXPECT_TRUE(known);

    EXPECT_EQ(service->deliver(ctx_for(root_id), "worker", "second", true), "");
    ASSERT_TRUE(wait_until([&] { return provider->payloads().back() == "second"; }));
    EXPECT_NE(registry->acquire(worker.session_id), nullptr);
    EXPECT_TRUE(any_content_contains(provider->request(provider->request_count() - 1),
                                     "answer:first"));
}

// 场景:用户在后台任务面板把已完成的子 agent「归档」收起(卸载会话 + meta.archived=true),
// 之后根 agent 又对它发 followup_task。
// 期望:子 agent 照常从磁盘恢复并带着原上下文工作,同时取消归档,让它重新出现在
// 后台任务面板里;归档只是收起,不影响 agent 树的寻址。
// 回归:若恢复时不取消归档,还在干活的 agent 在面板里永远看不到。
TEST_F(MeshAgentServiceTest, ArchivedAgentIsUnarchivedWhenAddressedAgain) {
    start_service();
    create_root();
    const auto worker = spawn(root_id, "worker", "first");
    ASSERT_EQ(worker.error, "");
    ASSERT_TRUE(wait_until([&] { return root_mail() == 1; }));
    ASSERT_TRUE(wait_until([&] { return !loop(worker.session_id)->is_busy(); }));

    // 模拟面板「归档」:Web 端先卸载会话,再把落盘 meta 标成 archived。
    registry->destroy(worker.session_id);
    const auto meta_path = acecode::SessionStorage::meta_path(
        acecode::SessionStorage::get_project_dir(workspace), worker.session_id);
    auto meta = acecode::SessionStorage::read_meta(meta_path);
    ASSERT_EQ(meta.id, worker.session_id);
    meta.archived = true;
    ASSERT_TRUE(acecode::SessionStorage::write_meta(meta_path, meta));

    EXPECT_EQ(service->deliver(ctx_for(root_id), "worker", "second", true), "");
    ASSERT_TRUE(wait_until([&] { return provider->payloads().back() == "second"; }));
    EXPECT_NE(registry->acquire(worker.session_id), nullptr);
    EXPECT_TRUE(any_content_contains(provider->request(provider->request_count() - 1),
                                     "answer:first"));
    EXPECT_FALSE(acecode::SessionStorage::read_meta(meta_path).archived)
        << "再次被寻址的 agent 要取消归档,重新出现在后台任务面板里";
}

// 场景:模型经工具调用整套协作工具(工具层的参数解析与输出格式)。
// 期望:输出逐字对齐 Codex —— spawn 回 {"task_name": 路径};send_message 成功输出为空;
// wait 回 {"message","timed_out"} 且按最小值夹取并附说明;超出最大值报错;
// 未知字段与 fork_context 被拒;list 返回 agents 数组。
TEST_F(MeshAgentServiceTest, ToolsMatchCodexArgumentAndOutputContracts) {
    config.swarm.mesh.min_wait_timeout_ms = 1000;
    start_service();
    acecode::ToolExecutor mesh_tools;
    acecode::register_mesh_agent_tools(mesh_tools, service, config);
    create_root();
    const auto ctx = ctx_for(root_id);

    auto waited = mesh_tools.execute("agent_wait", R"({"timeout_ms":5})", ctx);
    ASSERT_TRUE(waited.success) << waited.output;
    EXPECT_EQ(nlohmann::json::parse(waited.output),
              (nlohmann::json{{"message", "Wait timed out.\n\nRequested timeout of 5ms was "
                                          "clamped to the minimum of 1000ms."},
                              {"timed_out", true}}));
    auto too_long = mesh_tools.execute("agent_wait", R"({"timeout_ms":99999999})", ctx);
    EXPECT_FALSE(too_long.success);
    EXPECT_EQ(too_long.output, "timeout_ms must be at most 3600000");

    auto unknown = mesh_tools.execute(
        "agent_spawn", R"({"task_name":"w","message":"go","bogus":1})", ctx);
    EXPECT_FALSE(unknown.success);
    EXPECT_EQ(unknown.output, "unknown field `bogus`");
    auto fork_context = mesh_tools.execute(
        "agent_spawn", R"({"task_name":"w","message":"go","fork_context":true})", ctx);
    EXPECT_EQ(fork_context.output,
              "fork_context is not supported in MultiAgentV2; use fork_turns instead");
    auto bad_fork = mesh_tools.execute(
        "agent_spawn", R"({"task_name":"w","message":"go","fork_turns":"0"})", ctx);
    EXPECT_EQ(bad_fork.output, "fork_turns must be `none`, `all`, or a positive integer string");

    auto spawned = mesh_tools.execute("agent_spawn", R"({"task_name":"w","message":"go"})", ctx);
    ASSERT_TRUE(spawned.success) << spawned.output;
    EXPECT_EQ(spawned.output, R"({"task_name":"/root/w"})");
    EXPECT_FALSE(spawned.metadata.value("subagent_session_id", std::string{}).empty());
    ASSERT_TRUE(wait_until([&] { return root_mail() == 1; }));

    auto sent = mesh_tools.execute("agent_send_message", R"({"target":"w","message":"hi"})", ctx);
    ASSERT_TRUE(sent.success) << sent.output;
    EXPECT_EQ(sent.output, "");

    auto listed = mesh_tools.execute("agent_list", "{}", ctx);
    ASSERT_TRUE(listed.success) << listed.output;
    const auto agents = nlohmann::json::parse(listed.output).at("agents");
    ASSERT_EQ(agents.size(), 2u);
    EXPECT_EQ(agents[1].at("agent_name"), "/root/w");
    EXPECT_EQ(agents[1].at("agent_status"), (nlohmann::json{{"completed", "answer:go"}}));

    auto root_interrupt = mesh_tools.execute("agent_interrupt", R"({"target":"/root"})", ctx);
    EXPECT_FALSE(root_interrupt.success);
    EXPECT_EQ(root_interrupt.output, "root is not a spawned agent");
}
