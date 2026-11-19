#pragma once

// 「子进程有没有把父进程无关的文件句柄一并带走」的共用探针。
//
// MSVC 运行库打开的文件(std::ifstream / fopen)默认可继承,而且不允许别人删除或改名。
// 子进程以 bInheritHandles=TRUE 启动、又没有限定句柄清单时,这类句柄会被它带走并持有到
// 退出:父进程关掉文件之后,文件照样改不了名(Windows 错误 32)。
//
// 用法:构造探针(此刻文件已经被父进程读着)→ 启动被测子进程 → close_and_rename()。
// 子进程必须活过 close_and_rename(),否则句柄已经随它退出放掉,什么也测不出来:
// 调用方要另外断言那一刻子进程还在运行。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace acecode_test {

class InheritedFileProbe {
public:
    explicit InheritedFileProbe(const std::string& tag) {
        // 进程号 + 时刻 + 序号:ctest 并行跑多个测试进程时目录也不会撞。
        static std::atomic<unsigned> sequence{0};
#ifdef _WIN32
        const long long process_id = _getpid();
#else
        const long long process_id = getpid();
#endif
        root_ = std::filesystem::temp_directory_path() /
            ("acecode-" + tag + "-" + std::to_string(process_id) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence++));
        std::filesystem::create_directories(root_);
        file_ = root_ / "being-read.json";
        { std::ofstream out(file_, std::ios::binary); out << "{}"; }
        reader_.open(file_, std::ios::binary);
    }
    ~InheritedFileProbe() {
        reader_.close();
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    InheritedFileProbe(const InheritedFileProbe&) = delete;
    InheritedFileProbe& operator=(const InheritedFileProbe&) = delete;

    // 父进程是否正读着这个文件。
    bool reading() const { return reader_.is_open(); }
    // 探针自己的临时目录,测试要用的标记文件 / 假脚本也放这里,随探针一起清理。
    const std::filesystem::path& root() const { return root_; }

    // 父进程关掉文件后立刻给它改名。返回系统错误码:0 = 成功,
    // 32(ERROR_SHARING_VIOLATION)= 还有别的进程占着这个文件。
    int close_and_rename() {
        reader_.close();
        std::error_code ec;
        std::filesystem::rename(file_, root_ / "renamed.json", ec);
        return ec.value();
    }

private:
    std::filesystem::path root_;
    std::filesystem::path file_;
    std::ifstream reader_;
};

// 空转辅助进程(tests/platform/helpers/idle_child.cpp)的路径,UTF-8;Windows 下换成
// 反斜杠(构建系统给的是正斜杠,cmd.exe 会把不带引号的正斜杠当成开关)。
inline std::string idle_child_path() {
    std::string path = ACECODE_IDLE_TEST_CHILD_PATH;
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '/', '\\');
#endif
    return path;
}

} // namespace acecode_test
