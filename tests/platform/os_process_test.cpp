// 覆盖 daemon 进程抽象(src/base/platform/process/os_process_windows.cpp)里
// spawn_detached 的句柄继承范围。

#ifdef _WIN32

#include <gtest/gtest.h>

#include "platform/process/os_process.hpp"
#include "test_support/platform/inherited_file_probe.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

TEST(OsProcess, DetachedChildDoesNotInheritUnrelatedOpenFiles) {
    // 触发场景:TUI / CLI 拉起后台 daemon 或桌面版(spawn_detached)的那一刻,别的线程正用
    // std::ifstream 读文件(MSVC 运行库打开的句柄默认可继承,且不允许别人删除 / 改名)。
    // 被拉起的进程是常驻的,比拉起它的进程活得久。
    // 期望:后台进程只拿到重定向用的 NUL;父进程关掉文件后马上就能给它改名。
    // 回归:CreateProcessA 以 bInheritHandles=TRUE 启动且没有限定句柄清单,后台进程把那一刻
    // 所有可继承句柄都带走。实际表现:即使拉起它的进程早已退出,这个文件也一直改不了名、
    // 删不掉,报「另一个程序正在使用此文件」(错误 32),直到后台进程退出。
    acecode_test::InheritedFileProbe probe("detached-inherit");
    ASSERT_TRUE(probe.reading());

    const auto pid = acecode::daemon::spawn_detached({acecode_test::idle_child_path()});
    ASSERT_NE(pid, 0);
    HANDLE child = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    ASSERT_NE(child, nullptr) << "error " << ::GetLastError();

    const int rename_error = probe.close_and_rename();
    // 子进程还在跑,改名的结果才说明问题:它要是已经退出,带走的句柄也跟着放了。
    EXPECT_EQ(::WaitForSingleObject(child, 0), static_cast<DWORD>(WAIT_TIMEOUT));
    EXPECT_EQ(rename_error, 0);

    ::TerminateProcess(child, 0);
    ::WaitForSingleObject(child, 5000);
    ::CloseHandle(child);
}

#endif
