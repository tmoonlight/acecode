#include "test_support/agent_loop/characterization_fixture.hpp"

#include <iostream>

namespace {
using namespace acecode;
using namespace acecode_test::characterization;

struct HandoffGate {
    std::mutex mutex;
    std::condition_variable changed;
    int locked_sources = 0;
    int target_attempts = 0;
};

// 仅供 EXPECT_EXIT 子进程调用。两个 source.queue 都已拿到后,才准入相反方向的
// target.queue 请求;所有等待都有截止时间,僵持线程最后随子进程退出销毁。
[[noreturn]] void characterize_abba(const std::filesystem::path& root) {
    Isolation isolation(BorrowedProbeDirectory{root});
    Harness left(isolation, "handoff-left");
    Harness right(isolation, "handoff-right");
    auto gate = std::make_shared<HandoffGate>();
    // 两个并发调用共同持有 loop,保证回调内目标地址有效;父进程负责临时目录清理。
    auto left_loop = std::shared_ptr<AgentLoop>(std::move(left.loop));
    auto right_loop = std::shared_ptr<AgentLoop>(std::move(right.loop));
    const auto launch = [gate](std::shared_ptr<AgentLoop> source,
                               std::shared_ptr<AgentLoop> target,
                               std::string target_session) {
        return std::async(std::launch::async,
            [gate, source = std::move(source), target = std::move(target), target_session = std::move(target_session)] {
                return source->complete_task_handoff(target_session, [gate, target] {
                    {
                        std::unique_lock<std::mutex> lock(gate->mutex);
                        ++gate->locked_sources;
                        gate->changed.notify_all();
                        if (!gate->changed.wait_for(lock, 5s, [gate] { return gate->locked_sources == 2; })) {
                            std::_Exit(31);
                        }
                        ++gate->target_attempts;
                        gate->changed.notify_all();
                    }
                    target->enqueue_control([] { return true; });
                    return true;
                });
            });
    };
    auto ab = launch(left_loop, right_loop, right.session->current_session_id());
    auto ba = launch(right_loop, left_loop, left.session->current_session_id());
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        if (!gate->changed.wait_for(lock, 5s, [gate] { return gate->target_attempts == 2; })) std::_Exit(32);
    }
    if (ab.wait_for(200ms) != std::future_status::timeout ||
        ba.wait_for(200ms) != std::future_status::timeout) std::_Exit(33);
    std::cerr << "observed source.queue -> target.queue AB-BA" << std::endl;
    std::_Exit(0);
}

// 场景:两个空闲 loop 同时向对方交接,在 accept_target_input 中取目标队列门。
// 期望:记录当前 AB-BA 僵持,不在表征阶段改锁序。进程隔离与有界探针保证整个套件
// 不会永久等待;将来真正修复锁序时必须显式更新此已知问题断言。
TEST(AgentLoopHandoffGoldenDeathTest, OppositeHandoffsCurrentlyBlockAfterBothSourceLocks) {
    // 只有 GTest 确认的子进程借用父目录;普通运行忽略任何外部预设环境路径。
    // 子进程验证临时根和身份文件且从不删除,父作用域仅清理自己新建的目录。
    const bool child = testing::internal::InDeathTestChild();
    const char* inherited = child ? std::getenv("ACECODE_P011_ABBA_ROOT") : nullptr;
    ASSERT_FALSE(child && !inherited);
    std::optional<TemporaryDirectory> directory;
    if (child) directory.emplace(BorrowedProbeDirectory{path_from_utf8(inherited)});
    else directory.emplace();
    ScopedEnvironment root("ACECODE_P011_ABBA_ROOT", path_to_utf8(directory->path));
    const auto previous_style = testing::FLAGS_gtest_death_test_style;
    struct RestoreStyle {
        std::string previous;
        ~RestoreStyle() { testing::FLAGS_gtest_death_test_style = previous; }
    } restore{previous_style};
    testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT(characterize_abba(directory->path), testing::ExitedWithCode(0),
                "observed source.queue -> target.queue AB-BA");
}

// 场景:子探针借用目录或收到非探针路径。期望:借用析构保留父文件,非探针目录被拒绝;
// 临时目录只能由创建它的父作用域清理,不会把外部环境中的路径变成 remove_all 目标。
TEST(AgentLoopHandoffGolden, BorrowedProbeDirectoriesNeverAcquireCleanupOwnership) {
    TemporaryDirectory parent;
    const auto sentinel = parent.path / "parent-owned.txt";
    std::ofstream(sentinel) << "preserve parent ownership";
    {
        TemporaryDirectory borrowed(BorrowedProbeDirectory{parent.path});
        EXPECT_EQ(borrowed.path, parent.path);
    }
    EXPECT_TRUE(std::filesystem::is_regular_file(sentinel));
    const auto ordinary = parent.path / "ordinary";
    std::filesystem::create_directory(ordinary);
    EXPECT_THROW(TemporaryDirectory{BorrowedProbeDirectory{ordinary}}, std::runtime_error);
    EXPECT_TRUE(std::filesystem::is_directory(ordinary));
}

// 场景:单向交接目标拒绝或接受。期望:source 队列门在失败和成功后均可再取,
// 成功只投递一个目标 control;它与并发僵持用例共同区分普通交接和相反方向竞争。
TEST(AgentLoopHandoffGolden, OneWayHandoffReleasesSourceGateForFailureAndSuccess) {
    Isolation isolation;
    Harness source(isolation, "one-way-source");
    Harness target(isolation, "one-way-target");
    std::string error;
    EXPECT_FALSE(source.loop->complete_task_handoff(target.session->current_session_id(), [] { return false; }, &error));
    EXPECT_EQ(error, "target input was not accepted");
    EXPECT_TRUE(source.loop->try_run_idle_control([] {}));
    auto delivered = std::make_shared<std::promise<void>>();
    auto ready = delivered->get_future();
    EXPECT_TRUE(source.loop->complete_task_handoff(target.session->current_session_id(),
        [loop = target.loop.get(), delivered] {
            loop->enqueue_control([delivered] { delivered->set_value(); return true; });
            return true;
        }, &error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(ready.wait_for(5s), std::future_status::ready);
    EXPECT_TRUE(source.loop->try_run_idle_control([] {}));
}
} // namespace
