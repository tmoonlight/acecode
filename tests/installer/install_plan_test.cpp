// 覆盖 src/apps/installer/install_plan 的纯逻辑(不碰注册表和文件系统):
//   1. 命令行:--uninstall / --silent / --install-dir 两种写法 / --wait-pid / 未知参数报错。
//   2. 卸载副本重启参数带上原进程 PID 与安装目录,静默标志原样透传。
//   3. CreateProcess 参数加引号:空格、内嵌引号、末尾反斜杠。
//   4. 默认安装目录与「自选目录不以 ACECode 结尾就补子目录」。
//   5. 安装目录合法性:拒绝相对路径、盘符根、非法字符、ADS 冒号。
//   6. PATH 列表增删:大小写/引号/末尾反斜杠不敏感,其它条目逐字保留。
//   7. 卸载登记项:卸载命令带引号与 --uninstall,大小按 KB 向上取整。
//   8. 进度分段单调,版本比较按数字段而不是字符串。

#include "installer/install_plan.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace acecode::installer;

namespace {

const RegistryValue* find_value(const std::vector<RegistryValue>& values, const std::string& name) {
    const auto it = std::find_if(values.begin(), values.end(),
                                 [&name](const RegistryValue& v) { return v.name == name; });
    return it == values.end() ? nullptr : &*it;
}

}  // namespace

// 场景:「设置 > 应用」点卸载时的命令行 `uninstall.exe --uninstall --silent`,以及安装程序的
// `--install-dir=D:\Apps\ACECode` 与 `--install-dir D:\Apps\ACECode\` 两种写法。
// 期望:模式、静默标志正确;目录末尾反斜杠被去掉;--wait-pid 解析为数字。
TEST(InstallerArgs, ParsesModesAndDirectoryForms) {
    auto parsed = parse_installer_args({"--uninstall", "--silent"});
    ASSERT_TRUE(parsed.args.has_value()) << parsed.error;
    EXPECT_EQ(parsed.args->mode, InstallerMode::uninstall);
    EXPECT_TRUE(parsed.args->silent);

    parsed = parse_installer_args({"--install-dir=D:\\Apps\\ACECode"});
    ASSERT_TRUE(parsed.args.has_value());
    EXPECT_EQ(parsed.args->install_dir, "D:\\Apps\\ACECode");
    EXPECT_EQ(parsed.args->mode, InstallerMode::install);

    parsed = parse_installer_args({"--install-dir", "D:\\Apps\\ACECode\\", "--wait-pid", "4242",
                                   "--relaunched", "/S"});
    ASSERT_TRUE(parsed.args.has_value());
    EXPECT_EQ(parsed.args->install_dir, "D:\\Apps\\ACECode");
    EXPECT_EQ(parsed.args->wait_pid, 4242u);
    EXPECT_TRUE(parsed.args->relaunched);
    EXPECT_TRUE(parsed.args->silent);
}

// 场景:拼错的参数、缺值的 --install-dir、非数字 PID。
// 期望:返回错误文本而不是静默忽略(静默安装脚本写错时要能看到退出码 64)。
TEST(InstallerArgs, RejectsUnknownOrIncompleteArguments) {
    EXPECT_FALSE(parse_installer_args({"--instal-dir=C:\\x"}).args.has_value());
    EXPECT_FALSE(parse_installer_args({"--install-dir"}).args.has_value());
    EXPECT_FALSE(parse_installer_args({"--wait-pid", "12ab"}).args.has_value());
    EXPECT_FALSE(parse_installer_args({"--wait-pid", "99999999999"}).args.has_value());
}

// 场景:安装目录里的 uninstall.exe 被调起后,把自己复制到临时目录再带参数重启。
// 期望:副本参数里有 --relaunched、原 PID、安装目录;静默卸载时副本也保持静默。
TEST(InstallerArgs, RelaunchArgumentsCarryParentAndDirectory) {
    InstallerArgs args;
    args.mode = InstallerMode::uninstall;
    args.silent = true;
    const auto relaunch = uninstall_relaunch_args(args, 77, "C:\\Users\\a\\AppData\\Local\\Programs\\ACECode");
    const auto reparsed = parse_installer_args(relaunch);
    ASSERT_TRUE(reparsed.args.has_value()) << reparsed.error;
    EXPECT_EQ(reparsed.args->mode, InstallerMode::uninstall);
    EXPECT_TRUE(reparsed.args->relaunched);
    EXPECT_TRUE(reparsed.args->silent);
    EXPECT_EQ(reparsed.args->wait_pid, 77u);
    EXPECT_EQ(reparsed.args->install_dir, "C:\\Users\\a\\AppData\\Local\\Programs\\ACECode");
}

// 场景:目录里有空格、参数里有引号、目录以反斜杠结尾(CreateProcess 会把 `\"` 当成转义引号)。
// 期望:按 CommandLineToArgvW 的规则加引号,末尾反斜杠成对出现。
TEST(InstallerArgs, QuotesWindowsArguments) {
    EXPECT_EQ(quote_windows_arg("plain"), "plain");
    EXPECT_EQ(quote_windows_arg("C:\\Program Files\\ACECode"), "\"C:\\Program Files\\ACECode\"");
    EXPECT_EQ(quote_windows_arg("a \"b\""), "\"a \\\"b\\\"\"");
    EXPECT_EQ(quote_windows_arg("C:\\with space\\"), "\"C:\\with space\\\\\"");
    EXPECT_EQ(quote_windows_arg(""), "\"\"");
}

// 场景:没有旧安装时的默认目录;用户在选择器里选了 D:\Apps(一个已有的普通文件夹)。
// 期望:默认目录是 %LOCALAPPDATA%\Programs\ACECode;自选目录补出 ACECode 子目录,
// 选的本来就是 ...\ACECode 时不重复补。修复前若直接装进 D:\Apps,卸载会删掉整个 D:\Apps。
TEST(InstallerDirs, DefaultAndChosenDirectories) {
    EXPECT_EQ(default_install_dir("C:\\Users\\a\\AppData\\Local"),
              "C:\\Users\\a\\AppData\\Local\\Programs\\ACECode");
    EXPECT_EQ(normalize_chosen_install_dir("D:\\Apps"), "D:\\Apps\\ACECode");
    EXPECT_EQ(normalize_chosen_install_dir("D:/Apps/acecode/"), "D:\\Apps\\acecode");
    EXPECT_EQ(normalize_chosen_install_dir("E:\\"), "E:\\ACECode");
}

// 场景:各种不能当安装目录的输入。
// 期望:相对路径、盘符根、带 <>|?* 或第二个冒号(备用数据流)的路径都拒绝;正常盘符路径与 UNC 接受。
TEST(InstallerDirs, AcceptsOnlySafeAbsoluteDirectories) {
    EXPECT_TRUE(is_acceptable_install_dir("C:\\Users\\a\\AppData\\Local\\Programs\\ACECode"));
    EXPECT_TRUE(is_acceptable_install_dir("\\\\server\\share\\ACECode"));
    EXPECT_FALSE(is_acceptable_install_dir("ACECode"));
    EXPECT_FALSE(is_acceptable_install_dir("C:\\"));
    EXPECT_FALSE(is_acceptable_install_dir("C:"));
    EXPECT_FALSE(is_acceptable_install_dir("C:\\Apps\\AC|E"));
    EXPECT_FALSE(is_acceptable_install_dir("C:\\Apps\\ACECode:stream"));
    EXPECT_FALSE(is_acceptable_install_dir(""));
}

// 场景:用户 PATH 里已有大小写不同、带引号或末尾反斜杠的同一目录,以及别的目录与空条目。
// 期望:contains 能认出同一目录;with 不重复追加;without 只删掉匹配项,其它条目(含 %VAR% 与空条目)逐字保留。
TEST(InstallerPath, AddAndRemoveKeepOtherEntries) {
    const std::string dir = "C:\\Users\\a\\AppData\\Local\\Programs\\ACECode";
    const std::string current = "%USERPROFILE%\\bin;;\"c:\\users\\a\\appdata\\local\\programs\\acecode\\\";D:\\tools";
    EXPECT_TRUE(path_list_contains(current, dir));
    EXPECT_EQ(path_list_with(current, dir), current);
    EXPECT_EQ(path_list_without(current, dir), "%USERPROFILE%\\bin;;D:\\tools");

    EXPECT_EQ(path_list_with("", dir), dir);
    EXPECT_EQ(path_list_with("D:\\tools;", dir), "D:\\tools;" + dir);
    EXPECT_EQ(path_list_without("D:\\tools", dir), "D:\\tools");
    EXPECT_FALSE(path_list_contains("D:\\tools;C:\\Users\\a\\AppData\\Local\\Programs\\ACECode2", dir));
}

// 场景:为装在带空格目录里的 1.0.20 生成卸载登记项,文件总大小 1025 字节。
// 期望:UninstallString 是带引号的 uninstall.exe 加 --uninstall,静默卸载再加 --silent;
// EstimatedSize 是 KB 且向上取整为 2;不可修改 / 不可修复标志为 1;安装日期原样写入。
TEST(InstallerRegistry, UninstallEntryValues) {
    UninstallEntryInput input;
    input.install_dir = "D:\\My Apps\\ACECode\\";
    input.version = "1.0.20";
    input.installed_bytes = 1025;
    input.install_date = "20261105";
    const auto values = uninstall_entry_values(input);
    const auto* uninstall = find_value(values, "UninstallString");
    const auto* quiet = find_value(values, "QuietUninstallString");
    const auto* size = find_value(values, "EstimatedSize");
    const auto* location = find_value(values, "InstallLocation");
    const auto* icon = find_value(values, "DisplayIcon");
    ASSERT_TRUE(uninstall && quiet && size && location && icon);
    EXPECT_EQ(uninstall->text, "\"D:\\My Apps\\ACECode\\uninstall.exe\" --uninstall");
    EXPECT_EQ(quiet->text, "\"D:\\My Apps\\ACECode\\uninstall.exe\" --uninstall --silent");
    EXPECT_EQ(location->text, "D:\\My Apps\\ACECode");
    EXPECT_EQ(icon->text, "D:\\My Apps\\ACECode\\acecode-desktop.exe,0");
    EXPECT_TRUE(size->is_dword);
    EXPECT_EQ(size->number, 2u);
    ASSERT_NE(find_value(values, "NoModify"), nullptr);
    EXPECT_EQ(find_value(values, "NoModify")->number, 1u);
    ASSERT_NE(find_value(values, "InstallDate"), nullptr);
    EXPECT_EQ(find_value(values, "InstallDate")->text, "20261105");
    EXPECT_EQ(format_install_date(2026, 1, 9), "20260109");
}

// 场景:按步骤顺序上报进度,每步内部从 0 走到 1。
// 期望:整条进度单调不减、从 0 开始、完成时正好为 1;解压占最大的一段(当前包几十 MB,
// 其它步骤都在一秒内,按体感分配)。
TEST(InstallerProgress, StepsAreMonotonic) {
    double last = -1.0;
    for (InstallStep step : {InstallStep::preparing, InstallStep::extracting,
                             InstallStep::replacing, InstallStep::integrating}) {
        for (double f = 0.0; f <= 1.0; f += 0.25) {
            const double value = overall_progress(step, f);
            EXPECT_GE(value, last);
            last = value;
        }
    }
    EXPECT_DOUBLE_EQ(overall_progress(InstallStep::preparing, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(overall_progress(InstallStep::finished, 0.0), 1.0);
    EXPECT_GT(overall_progress(InstallStep::extracting, 1.0) -
                  overall_progress(InstallStep::extracting, 0.0),
              0.5);
}

// 场景:已装 1.0.9、安装包 1.0.20;同版本重装;已装比安装包新。
// 期望:按数字段比较(字符串比较会把 1.0.9 当成更新),按钮分别是「更新」「重新安装」「安装」。
TEST(InstallerVersions, ClassifiesUpdateReinstallAndInstall) {
    EXPECT_LT(compare_versions("1.0.9", "1.0.20"), 0);
    EXPECT_EQ(compare_versions("1.0.20", "1.0.20.0"), 0);
    EXPECT_GT(compare_versions("2.0", "1.99.99"), 0);
    EXPECT_EQ(classify_install(std::string("1.0.9"), "1.0.20"), InstallAction::update);
    EXPECT_EQ(classify_install(std::string("1.0.20"), "1.0.20"), InstallAction::reinstall);
    EXPECT_EQ(classify_install(std::string("1.1.0"), "1.0.20"), InstallAction::install);
    EXPECT_EQ(classify_install(std::nullopt, "1.0.20"), InstallAction::install);
}
