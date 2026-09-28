#include <gtest/gtest.h>
#include "platform/process/unique_resources.hpp"
#include "platform/process/piped_process.hpp"
#include "platform/unique_sqlite.hpp"
#include <sqlite3.h>
#include <stdexcept>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace acecode::platform;

static_assert(!std::is_copy_constructible_v<UniqueFd>);
static_assert(!std::is_copy_assignable_v<UniqueProcess>);
static_assert(!std::is_copy_constructible_v<UniqueSqlite>);

TEST(UniqueResources, SqliteMoveKeepsConnectionUntilFinalOwnerCloses) {
    // 场景:数据库连接移动后原包装析构。期望新所有者仍可查询;
    // 原来的裸指针无法表达转移,局部可复制包装可能重复关闭连接。
    UniqueSqlite db;
    ASSERT_EQ(sqlite3_open(":memory:", db.put()), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db.get(), "CREATE TABLE item(id INTEGER);", nullptr, nullptr, nullptr), SQLITE_OK);
    UniqueSqlite moved(std::move(db));
    EXPECT_FALSE(db);
    ASSERT_EQ(sqlite3_exec(moved.get(), "INSERT INTO item VALUES(1);", nullptr, nullptr, nullptr), SQLITE_OK);
    UniqueSqlite target;
    target = std::move(moved);
    EXPECT_FALSE(moved);
    EXPECT_EQ(sqlite3_exec(target.get(), "SELECT * FROM item;", nullptr, nullptr, nullptr), SQLITE_OK);
    target.reset();
    EXPECT_FALSE(target);
}

#ifdef _WIN32
static_assert(!std::is_copy_constructible_v<UniqueHandle>);
static_assert(!std::is_copy_constructible_v<UniqueLocalMem>);
static_assert(!std::is_copy_constructible_v<UniqueSid>);

TEST(UniqueResources, HandleMoveAndExceptionCloseExactlyOnce) {
    // 场景:创建句柄并转移所有权后抛异常。期望移动源不关闭句柄,
    // 栈展开时唯一所有者释放;原局部 Handle 可复制,异常路径存在重复关闭风险。
    HANDLE observed = nullptr;
    try {
        UniqueHandle original(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ASSERT_TRUE(original);
        observed = original.get();
        UniqueHandle owner(std::move(original));
        EXPECT_FALSE(original);
        EXPECT_TRUE(SetEvent(owner.get()));
        EXPECT_EQ(WaitForSingleObject(owner.get(), 0), WAIT_OBJECT_0);
        throw std::runtime_error("unwind");
    } catch (const std::runtime_error&) {}
    DWORD flags = 0;
    EXPECT_FALSE(GetHandleInformation(observed, &flags));
    EXPECT_EQ(GetLastError(), ERROR_INVALID_HANDLE);
}

TEST(UniqueResources, RepeatedFailedSpawnDoesNotLeakPipeHandles) {
    // 场景:两组管道已创建,但可执行文件不存在。期望每次失败自动释放全部管道;
    // 原实现依赖各出口手工 cleanup,新增早退或异常可能遗漏释放。
    SpawnOptions options;
    options.argv = {"Z:\\acecode-no-such-executable-ownership-test.exe"};
    std::string error;
    { PipedProcess warmup; EXPECT_FALSE(warmup.start(options, &error)); }
    DWORD before = 0, after = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (int i = 0; i < 64; ++i) {
        PipedProcess process;
        EXPECT_FALSE(process.start(options, &error));
        EXPECT_FALSE(process.started());
    }
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &after));
    EXPECT_LE(after, before + 2); // 容忍同进程测试基础设施的少量临时句柄波动。
}

TEST(UniqueResources, ProcessOwnerKillsSuspendedChildOnException) {
    // 场景:进程创建成功后装配抛异常。期望所有者析构终止并等待子进程;
    // 原裸进程句柄在未进入显式 terminate 路径时无法提供这个保证。
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::wstring command = L"cmd.exe /d /c exit 0";
    ASSERT_TRUE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info));
    UniqueHandle thread(info.hThread);
    UniqueHandle observer;
    {
        UniqueProcess process(info.hProcess);
        ASSERT_TRUE(DuplicateHandle(GetCurrentProcess(), process.get(), GetCurrentProcess(),
            observer.put(), SYNCHRONIZE, FALSE, 0));
        EXPECT_EQ(WaitForSingleObject(observer.get(), 0), WAIT_TIMEOUT);
        UniqueProcess moved(std::move(process));
        EXPECT_EQ(process.get(), nullptr);
    }
    EXPECT_EQ(WaitForSingleObject(observer.get(), 0), WAIT_OBJECT_0);
}
#else
TEST(UniqueResources, FdMoveAndExceptionReleaseThePipe) {
    // 场景:管道端点移动后抛异常。期望唯一所有者在栈展开时关闭;
    // 原可复制局部 Fd 会让多份包装关闭同一描述符。
    int descriptors[2];
    ASSERT_EQ(pipe(descriptors), 0);
    UniqueFd writer(descriptors[1]);
    const int observed = descriptors[0];
    try {
        UniqueFd reader(observed);
        UniqueFd moved(std::move(reader));
        EXPECT_FALSE(reader);
        throw std::runtime_error("unwind");
    } catch (const std::runtime_error&) {}
    EXPECT_EQ(fcntl(observed, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}
#endif
