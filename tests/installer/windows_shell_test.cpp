// 覆盖 src/apps/installer/windows_shell 里需要 COM 的快捷方式接口(仅 Windows)。
//   1. 在一条从未初始化过 COM 的线程上创建快捷方式并读回目标。安装界面把安装放在工作线程上跑,
//      正是这种线程。

#include "installer/windows_shell.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace fs = std::filesystem;
namespace shell = acecode::installer::shell;

#ifdef _WIN32

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

// 场景:新开一条线程(不调用 CoInitializeEx),在临时目录里建一个指向当前测试程序的快捷方式,
// 再用 shortcut_target 读回目标。
// 期望:创建成功、读回的目标就是测试程序。
// 回归:1.0.21 的安装界面在工作线程上建开始菜单快捷方式,那条线程没进过 COM 套间;开发机上
// 常有别的组件先建好多线程套间,线程被隐式并进去所以没事,干净的机器上 CoCreateInstance 返回
// CO_E_NOTINITIALIZED,安装停在「failed to create a shell link object」,重试也一样;
// 卸载时 shortcut_target 同样失败,快捷方式被静默留下。
TEST(InstallerShell, ShortcutsWorkOnThreadWithoutCom) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() / ("acecode-shell-test-" + std::to_string(stamp));
    fs::create_directories(dir);
    const std::string link = acecode::path_to_utf8(dir / "ACECode Test.lnk");
    const std::string target = shell::current_executable_path();
    ASSERT_FALSE(target.empty());

    bool created = false;
    std::string error;
    std::optional<std::string> read_back;
    std::thread worker([&link, &target, &dir, &created, &error, &read_back] {
        created = shell::create_shortcut(link, target, acecode::path_to_utf8(dir), "test", &error);
        read_back = shell::shortcut_target(link);
    });
    worker.join();

    EXPECT_TRUE(created) << error;
    ASSERT_TRUE(read_back.has_value());
    EXPECT_EQ(lower(*read_back), lower(target));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#endif
