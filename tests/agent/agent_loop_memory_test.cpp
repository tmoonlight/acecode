// 覆盖 AgentLoop 侧的记忆快照与会话开关(openspec unify-memory-system 5.1 / 6.4):
// - 会话第一次请求生成记忆快照,之后逐字节复用;会话中途 memory_write 不改变本会话
//   已注入的内容(不打穿 prompt cache),压缩之后按磁盘重建
// - /memory off(SessionManager::set_memory_enabled(false)):请求不含记忆上下文、
//   模型侧工具表没有记忆工具、静态提示没有 # Memory 指引;随 meta 持久化,恢复会话后
//   保持;/memory on 后恢复

#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "memory/memory_service.hpp"
#include "permissions/permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "test_support/agent/agent_loop_fixture.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "tool/memory_read_tool.hpp"
#include "tool/memory_write_tool.hpp"
#include "tool/tool_executor.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace std::chrono_literals;

namespace {

using acecode_test::MemoryTestHome;

// 压缩走非流式 chat:返回一段摘要,让摘要压缩真正成功(而不是退化成机械修剪)。
class CompactingStub : public acecode_test::StubLlmProvider {
public:
    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>&,
                               const std::vector<acecode::ToolDef>&) override {
        acecode::ChatResponse response;
        response.content = "Summary of the earlier conversation.";
        response.finish_reason = "stop";
        return response;
    }
};

void wait_done(acecode::AgentLoop& loop, const std::function<void()>& action) {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    const auto sub = loop.events().subscribe([&](const acecode::SessionEvent& event) {
        if (event.kind != acecode::SessionEventKind::Done) return;
        std::lock_guard<std::mutex> lock(mu);
        done = true;
        cv.notify_all();
    });
    action();
    std::unique_lock<std::mutex> lock(mu);
    EXPECT_TRUE(cv.wait_for(lock, 10s, [&] { return done; }));
    lock.unlock();
    loop.events().unsubscribe(sub);
}

// 请求里那条 <system-reminder> 会话上下文消息(没有就返回空串)。
std::string session_context(const std::vector<acecode::ChatMessage>& request) {
    for (const auto& message : request) {
        if (message.role == "user" && message.content.find("<system-reminder>") != std::string::npos) {
            return message.content;
        }
    }
    return {};
}

bool has_tool(const std::vector<acecode::ToolDef>& tools, const std::string& name) {
    for (const auto& tool : tools) {
        if (tool.name == name) return true;
    }
    return false;
}

struct MemoryLoopHarness {
    explicit MemoryLoopHarness(MemoryTestHome& home)
        : cwd(home.workspace_cwd("ws")), memory(home.service()), provider(std::make_shared<CompactingStub>()) {
        tools.register_tool(acecode::create_memory_read_tool(memory));
        tools.register_tool(acecode::create_memory_write_tool(memory));
        session.start_session(cwd, "stub", "stub-model", acecode::SessionStorage::generate_session_id());
        auto services = acecode_test::AgentLoopFixture::dependencies(
            [p = provider]() -> std::shared_ptr<acecode::LlmProvider> { return p; },
            tools, {}, permissions, &session, nullptr, memory);
        loop = std::make_unique<acecode::AgentLoop>(services,
            acecode_test::AgentLoopFixture::configuration(cwd));
        loop->start();
        loop->set_context_window(128000);
    }
    ~MemoryLoopHarness() {
        loop->shutdown();
        session.finalize();
    }

    void turn(const std::string& text) {
        provider->push_text("ok");
        wait_done(*loop, [&] { loop->submit(text); });
    }

    void write_global(const std::string& name) {
        std::string err;
        ASSERT_TRUE(memory->global().upsert(name, acecode::MemoryType::User, "rule " + name, "b\n",
                                            acecode::MemoryWriteMode::Upsert, err).has_value()) << err;
    }

    std::string cwd;
    std::shared_ptr<acecode::MemoryService> memory;
    std::shared_ptr<CompactingStub> provider;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::SessionManager session;
    std::unique_ptr<acecode::AgentLoop> loop;
};

} // namespace

// 场景:会话进行中经 memory_write 新增了一条记忆,随后继续对话;之后手动压缩。
// 期望:压缩前后续请求里的记忆上下文与写入前逐字节相同、不含新条目;压缩之后的
// 请求按磁盘重建,出现新条目。
// 回归背景:旧实现每次请求都按 MEMORY.md 内容重建上下文,会话中途的写入(以及别的
// 进程 / 记忆摘要的写入)会在回合中途改变请求前缀,打穿 prompt cache。
TEST(AgentLoopMemoryTest, SnapshotIsFrozenUntilCompaction) {
    MemoryTestHome home("agentloop-memory-frozen");
    MemoryLoopHarness h(home);
    h.write_global("first_rule");

    h.turn("hello");
    const std::string first = session_context(h.provider->messages_for_turn(0));
    ASSERT_NE(first.find("first_rule"), std::string::npos) << first;

    h.write_global("second_rule");
    h.turn("again");
    const std::string second = session_context(h.provider->messages_for_turn(1));
    EXPECT_EQ(second, first);
    EXPECT_EQ(second.find("second_rule"), std::string::npos);

    wait_done(*h.loop, [&] { h.loop->submit_compact(); });
    h.turn("after compaction");
    const int last = h.provider->turn_count() - 1;
    const std::string rebuilt = session_context(h.provider->messages_for_turn(last));
    EXPECT_NE(rebuilt.find("second_rule"), std::string::npos) << rebuilt;
}

// 场景:用户执行 /memory off 后继续对话;再执行 /memory on。
// 期望:关闭期间请求不含记忆上下文、工具表没有 memory_read / memory_write、静态提示
// 没有 # Memory 指引;开启后三者都恢复。
TEST(AgentLoopMemoryTest, SessionMemoryOffRemovesContextAndTools) {
    MemoryTestHome home("agentloop-memory-off");
    MemoryLoopHarness h(home);
    h.write_global("visible_rule");

    h.session.set_memory_enabled(false);
    h.turn("memory off");
    const auto off_request = h.provider->messages_for_turn(0);
    EXPECT_EQ(session_context(off_request).find("visible_rule"), std::string::npos);
    EXPECT_FALSE(has_tool(h.provider->tools_for_turn(0), "memory_read"));
    EXPECT_FALSE(has_tool(h.provider->tools_for_turn(0), "memory_write"));
    ASSERT_FALSE(off_request.empty());
    EXPECT_EQ(off_request.front().content.find("# Memory\n"), std::string::npos);

    h.session.set_memory_enabled(true);
    h.turn("memory on");
    const auto on_request = h.provider->messages_for_turn(1);
    EXPECT_NE(session_context(on_request).find("visible_rule"), std::string::npos);
    EXPECT_TRUE(has_tool(h.provider->tools_for_turn(1), "memory_read"));
    EXPECT_TRUE(has_tool(h.provider->tools_for_turn(1), "memory_write"));
    EXPECT_NE(on_request.front().content.find("# Memory\n"), std::string::npos);
}

// 场景:会话执行 /memory off 后被关闭,之后恢复这个会话。
// 期望:开关随 meta 持久化(memory_mode: off),恢复后仍是关闭。
TEST(AgentLoopMemoryTest, SessionMemoryOffSurvivesResume) {
    MemoryTestHome home("agentloop-memory-resume");
    std::string id;
    const std::string cwd = home.workspace_cwd("ws");
    {
        acecode::SessionManager session;
        session.start_session(cwd, "stub", "stub-model", acecode::SessionStorage::generate_session_id());
        acecode::ChatMessage user;
        user.role = "user";
        user.content = "hello";
        session.on_message(user);
        session.set_memory_enabled(false);
        id = session.current_session_id();
        session.finalize();
    }
    const auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(MemoryTestHome::project_dir(cwd), id));
    EXPECT_EQ(meta.memory_mode, "off");

    acecode::SessionManager resumed;
    resumed.start_session(cwd, "stub", "stub-model");
    resumed.resume_session(id);
    EXPECT_FALSE(resumed.memory_enabled());
    resumed.set_memory_enabled(true);
    EXPECT_TRUE(resumed.memory_enabled());
    resumed.finalize();
}
