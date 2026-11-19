// 覆盖 bash 工具的子进程启动(src/adapters/tool/shell_process.cpp)的句柄继承范围。
// 受限令牌分支(CreateProcessAsUserW)的同款用例在 tests/sandbox/sandbox_backend_win_test.cpp,
// 那边有准备沙盒请求需要的支撑代码。

#ifdef _WIN32

#include <gtest/gtest.h>

#include "environment/terminal_runtime.hpp"
#include "test_support/platform/inherited_file_probe.hpp"
#include "tool/shell_process.hpp"

#include <string>

namespace {

// 没有发布过终端快照时 launch 走 `cmd.exe /d /c <命令>`,命令行的写法才是确定的。
// 别的用例可能已经发布过 PowerShell / Git Bash 快照,这里先清掉、结束时还原。
struct ScopedDefaultTerminal {
    decltype(acecode::environment::terminal().last()) snapshot;
    ScopedDefaultTerminal() : snapshot(acecode::environment::terminal().last()) {
        acecode::environment::terminal().reset_for_test();
    }
    ~ScopedDefaultTerminal() {
        if (snapshot) acecode::environment::terminal().publish(*snapshot);
        else acecode::environment::terminal().reset_for_test();
    }
};

} // namespace

TEST(ShellProcess, ChildDoesNotInheritUnrelatedOpenFiles) {
    // 触发场景:bash 工具起一条长命令(dev server、watch、转到后台的任务)的那一刻,
    // 别的线程正用 std::ifstream 读文件(MSVC 运行库打开的句柄默认可继承,且不允许
    // 别人删除 / 改名)。
    // 期望:命令进程只拿到自己的输出管道;父进程关掉文件后马上就能给它改名。
    // 回归:CreateProcessW 以 bInheritHandles=TRUE 启动且没有限定句柄清单,命令进程
    // (连同它再起的子进程)把那一刻所有可继承句柄都带走。实际表现:这个文件在命令结束
    // 之前一直改不了名、删不掉,报「另一个程序正在使用此文件」(错误 32)。
    ScopedDefaultTerminal terminal;
    acecode_test::InheritedFileProbe probe("shell-inherit");
    ASSERT_TRUE(probe.reading());

    acecode::ShellLaunchSpec spec;
    spec.command = "\"" + acecode_test::idle_child_path() + "\"";
    acecode::ShellLaunchFailure failure;
    auto process = acecode::ShellProcess::launch(spec, failure);
    ASSERT_NE(process, nullptr) << failure.message;

    const int rename_error = probe.close_and_rename();
    // 命令还在跑,改名的结果才说明问题:它要是已经退出,带走的句柄也跟着放了。
    EXPECT_FALSE(process->poll_exited());
    EXPECT_EQ(rename_error, 0);
    process->kill_tree();
}

#endif
