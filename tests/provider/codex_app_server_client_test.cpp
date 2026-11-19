// 覆盖 Codex app-server 客户端起子进程(src/adapters/provider/codex/codex_app_server_client.cpp)
// 的句柄继承范围。客户端的命令行写死为 `cmd.exe /d /c codex app-server …`,测试在 PATH
// 最前面放一个假的 codex.cmd,让它转去跑空转辅助进程,不需要本机装有 codex。

#ifdef _WIN32

#include <gtest/gtest.h>

#include "provider/codex/codex_app_server_client.hpp"
#include "test_support/platform/inherited_file_probe.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

// 把一个目录放到本进程 PATH 的最前面,析构时原样还原。子进程默认继承这份环境。
class ScopedPathPrefix {
public:
    explicit ScopedPathPrefix(const std::filesystem::path& directory) {
        const DWORD size = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (size > 0) {
            std::vector<wchar_t> buffer(size);
            const DWORD written = ::GetEnvironmentVariableW(L"PATH", buffer.data(), size);
            original_.assign(buffer.data(), written);
            had_original_ = true;
        }
        const std::wstring prefixed = directory.wstring() + L";" + original_;
        ::SetEnvironmentVariableW(L"PATH", prefixed.c_str());
    }
    ~ScopedPathPrefix() {
        ::SetEnvironmentVariableW(L"PATH", had_original_ ? original_.c_str() : nullptr);
    }
    ScopedPathPrefix(const ScopedPathPrefix&) = delete;
    ScopedPathPrefix& operator=(const ScopedPathPrefix&) = delete;

private:
    std::wstring original_;
    bool had_original_ = false;
};

} // namespace

TEST(CodexAppServerClient, ServerChildDoesNotInheritUnrelatedOpenFiles) {
    // 触发场景:Codex provider 第一次用到时拉起 `codex app-server` 的那一刻,别的线程正用
    // std::ifstream 读文件(MSVC 运行库打开的句柄默认可继承,且不允许别人删除 / 改名)。
    // app-server 是常驻的,和客户端同寿命。
    // 期望:app-server 进程只拿到自己的三个标准流;宿主关掉文件后马上就能给它改名。
    // 回归:CreateProcessA 以 bInheritHandles=TRUE 启动且没有限定句柄清单,cmd.exe 和它起的
    // app-server 把那一刻所有可继承句柄都带走。实际表现:这个文件一直改不了名、删不掉,
    // 报「另一个程序正在使用此文件」(错误 32),直到 app-server 退出。
    acecode_test::InheritedFileProbe probe("codex-inherit");
    ASSERT_TRUE(probe.reading());

    // 假的 codex:读 stdin 到 EOF 才退出。客户端 stop() 只结束 cmd.exe 并关掉 stdin 写端,
    // 这样留下的辅助进程会跟着退出,不会占着 stdout 管道让读线程等不到 EOF。
    const auto bin = probe.root() / "bin";
    std::filesystem::create_directories(bin);
    {
        std::ofstream script(bin / "codex.cmd", std::ios::binary);
        script << "@\"" << acecode_test::idle_child_path() << "\" --until-stdin-eof\r\n";
    }
    ScopedPathPrefix path(bin);

    acecode::codex::AppServerClient client;
    std::string error;
    ASSERT_TRUE(client.start(&error)) << error;

    const int rename_error = probe.close_and_rename();
    // 读线程还没读到 EOF,说明子进程还活着:它要是已经退出,带走的句柄也跟着放了。
    EXPECT_TRUE(client.running());
    EXPECT_EQ(rename_error, 0);
    client.stop();
}

#endif
