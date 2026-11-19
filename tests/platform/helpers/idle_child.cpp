// 测试用的空转子进程:什么都不做,只是活着。「起子进程时有没有把父进程无关的句柄
// 一并带走」这类测试需要一个确定还在运行的子进程,而且不能依赖外部命令的行为。
//
//   idle_test_child [任意参数]
//       空转,最多 30 秒。环境变量 ACECODE_IDLE_CHILD_STOP_FILE 指向的文件一出现就退出,
//       供拿不到进程句柄的测试收场。参数一律忽略,所以命令行由被测代码写死的启动点
//       (桌面版起 daemon、升级 runner)也能直接拿它顶替真正的可执行文件。
//   idle_test_child --until-stdin-eof
//       读 stdin 到 EOF 再退出。给走管道的启动点用:父进程关掉写端它就走,
//       不会留下一个孤儿继续占着 stdout 管道。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <thread>

namespace {

std::filesystem::path stop_file_from_environment() {
#ifdef _WIN32
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, L"ACECODE_IDLE_CHILD_STOP_FILE") != 0 || !value) return {};
    std::filesystem::path path(value);
    std::free(value);
    return path;
#else
    const char* value = std::getenv("ACECODE_IDLE_CHILD_STOP_FILE");
    return value ? std::filesystem::path(value) : std::filesystem::path();
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--until-stdin-eof") == 0) {
        while (std::fgetc(stdin) != EOF) {
        }
        return 0;
    }

    const auto stop_file = stop_file_from_environment();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        if (!stop_file.empty() && std::filesystem::exists(stop_file, ec)) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return 0;
}
