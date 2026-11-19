// 覆盖桌面版起 daemon 子进程(src/apps/desktop/daemon_supervisor.cpp)的句柄继承范围。
// 用空转辅助进程顶替 acecode.exe:supervisor 自己拼的 daemon 参数它一律忽略,只负责活着。

#ifdef _WIN32

#include <gtest/gtest.h>

#include "desktop/daemon_supervisor.hpp"
#include "test_support/platform/inherited_file_probe.hpp"

#include <string>

TEST(DaemonSupervisor, DaemonChildDoesNotInheritUnrelatedOpenFiles) {
    // 触发场景:桌面版为某个 workspace 起 daemon 的那一刻,别的线程正用 std::ifstream 读文件
    // (MSVC 运行库打开的句柄默认可继承,且不允许别人删除 / 改名)。daemon 与桌面版同寿命。
    // 期望:daemon 只拿到重定向用的 NUL;桌面版关掉文件后马上就能给它改名。
    // 回归:CreateProcessW 以 bInheritHandles=TRUE 启动且没有限定句柄清单,daemon 把那一刻
    // 所有可继承句柄都带走。实际表现:桌面版启动时读过的文件(配置、状态、安装目录里的
    // 资源)只要撞上这个时机,就一直改不了名、删不掉,报「另一个程序正在使用此文件」
    // (错误 32),要等这个 workspace 的 daemon 退出才恢复。
    acecode_test::InheritedFileProbe probe("daemon-supervisor-inherit");
    ASSERT_TRUE(probe.reading());

    acecode::desktop::DaemonSupervisor supervisor;
    acecode::desktop::SpawnRequest request;
    request.daemon_exe_path = acecode_test::idle_child_path();
    request.port = 1;             // 空转进程不监听端口,这两项只是过参数校验。
    request.token = "test-token";
    const auto spawned = supervisor.spawn(request);
    // supervisor 要把子进程挂进自己的 Job;宿主本身跑在不许嵌套的 Job 里时挂不上,
    // 那不是这条用例要测的东西。
    if (!spawned.ok && spawned.error.find("AssignProcessToJobObject") != std::string::npos) {
        GTEST_SKIP() << "nested job assignment refused on this host: " << spawned.error;
    }
    ASSERT_TRUE(spawned.ok) << spawned.error;

    const int rename_error = probe.close_and_rename();
    // 子进程还在跑,改名的结果才说明问题:它要是已经退出,带走的句柄也跟着放了。
    EXPECT_TRUE(supervisor.running());
    EXPECT_EQ(rename_error, 0);
    supervisor.stop();
}

#endif
