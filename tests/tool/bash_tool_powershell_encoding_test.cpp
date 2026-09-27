// bash 工具在真实 Windows PowerShell 5.1 下执行命令的编码集成测试
// (fix-feedback-0924 第 4 条)。
//
// 现场:agent 在 Windows PowerShell 5.1 下用 `Get-Content -Raw` 读无 BOM 的 UTF-8
// 源码(profit.vue),5.1 按 ANSI 代码页(中文系统 = GBK)解码;再用
// `[IO.File]::WriteAllText` 写回,整份中文被永久写坏,随后一个回合 110 次迭代都在
// 猜原文案。`$x = git show ...` 截获原生命令输出时按 [Console]::OutputEncoding
// 解码,bash 工具开的隐藏控制台是 936,同样乱码。
//
// 这些用例只在 Windows 本机上有意义(CI 没有 Windows 单测任务),非 Windows 下整个
// 文件是空翻译单元。找不到 powershell.exe 时跳过。

#ifdef _WIN32

#include <gtest/gtest.h>

#include "environment/terminal_runtime.hpp"
#include "tool/bash_tool.hpp"
#include "utils/utf8_path.hpp"
#include "test_support/sandbox/test_support.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;

// 「中文 profit 标题」的 UTF-8 字节,外加换行;用字节写死,不依赖源文件编码。
const std::string kUtf8Line =
    "\xE4\xB8\xAD\xE6\x96\x87 profit \xE6\xA0\x87\xE9\xA2\x98\n";
const std::string kUtf8Text = "\xE4\xB8\xAD\xE6\x96\x87 profit \xE6\xA0\x87\xE9\xA2\x98";

std::optional<std::string> windows_powershell_path() {
    wchar_t system_dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::nullopt;
    const fs::path exe =
        fs::path(system_dir) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    std::error_code ec;
    if (!fs::exists(exe, ec)) return std::nullopt;
    return acecode::path_to_utf8(exe);
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

// PowerShell 单引号字符串字面量(路径里的单引号加倍)。
std::string ps_quote(const fs::path& path) {
    std::string s = acecode::path_to_utf8(path);
    std::string out = "'";
    for (char c : s) {
        out.push_back(c);
        if (c == '\'') out.push_back('\'');
    }
    out.push_back('\'');
    return out;
}

class BashToolPowerShellEncodingTest : public testing::Test {
protected:
    void SetUp() override {
        previous_ = acecode::environment::terminal().last();
        const auto exe = windows_powershell_path();
        if (!exe) GTEST_SKIP() << "powershell.exe not found";
        acecode::environment::TerminalResolution resolution;
        resolution.resolved.id = "powershell";
        resolution.resolved.family = acecode::environment::TerminalFamily::PowerShell;
        resolution.resolved.program = *exe;
        resolution.resolved.usable = true;
        acecode::environment::terminal().publish(resolution);
        write_bytes(tree_.root / "u8.txt", kUtf8Line);
    }
    void TearDown() override {
        if (previous_) acecode::environment::terminal().publish(*previous_);
        else acecode::environment::terminal().reset_for_test();
    }

    acecode::ToolResult run(const std::string& command) {
        acecode::ToolContext context;
        context.cwd = acecode::path_to_utf8(tree_.root);
        return acecode::create_bash_tool().execute(
            nlohmann::json{{"command", command}}.dump(), context);
    }

    acecode::sandbox::test::TempTree tree_;

private:
    std::optional<acecode::environment::TerminalResolution> previous_;
};

} // namespace

// 场景:5.1 下 Get-Content -Raw 读无 BOM 的 UTF-8 文件。
// 期望:输出里是原文中文;stderr 没有 CLIXML 进度记录。
// 回归表现:修复前输出是 GBK 解码出的乱码「涓枃 profit 鏍囬」。
TEST_F(BashToolPowerShellEncodingTest, ReadsUtf8FileWithoutBomAsUtf8) {
    const auto result = run("Get-Content -Raw " + ps_quote(tree_.root / "u8.txt"));
    EXPECT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find(kUtf8Text), std::string::npos) << result.output;
    EXPECT_EQ(result.output.find("#< CLIXML"), std::string::npos) << result.output;
}

// 场景:复现反馈现场 —— Get-Content -Raw 读、WriteAllText(无 BOM UTF-8)写回。
// 期望:写回的文件与原文件逐字节相同。
// 回归表现:修复前整份中文被写成乱码,且无法从乱码恢复原文。
TEST_F(BashToolPowerShellEncodingTest, RoundTripThroughWriteAllTextPreservesBytes) {
    const fs::path out = tree_.root / "out.txt";
    const auto result = run(
        "$c = Get-Content -Raw " + ps_quote(tree_.root / "u8.txt") + "; " +
        "[IO.File]::WriteAllText(" + ps_quote(out) + ", $c, [Text.UTF8Encoding]::new($false))");
    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(read_bytes(out), kUtf8Line);
}

// 场景:截获原生命令输出(`cmd /c type` 原样输出 UTF-8 字节,作为 `git show` 的替身)。
// 期望:按 UTF-8 解码,中文正确。pwsh 7 在修复前同样失败(隐藏控制台是 936)。
TEST_F(BashToolPowerShellEncodingTest, CapturedNativeOutputDecodesAsUtf8) {
    const auto result = run(
        "$x = cmd /c type " + ps_quote(tree_.root / "u8.txt") + "; $x");
    EXPECT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find(kUtf8Text), std::string::npos) << result.output;
}

// 场景:5.1 下 Set-Content 写中文。期望:按 UTF-8(带 BOM,5.1 的 -Encoding UTF8 语义,
// 用户拍板的取舍)写出,而不是 ANSI。
TEST_F(BashToolPowerShellEncodingTest, SetContentWritesUtf8) {
    const fs::path out = tree_.root / "set.txt";
    const auto result = run(
        "Get-Content -Raw " + ps_quote(tree_.root / "u8.txt") +
        " | Set-Content -NoNewline " + ps_quote(out));
    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(read_bytes(out), "\xEF\xBB\xBF" + kUtf8Line);
}

// 场景:前置脚本不应改变退出码语义。期望:原生命令的退出码经 $LASTEXITCODE 透传。
TEST_F(BashToolPowerShellEncodingTest, ExitCodeIsPreserved) {
    const auto result = run("cmd /c exit 5; exit $LASTEXITCODE");
    EXPECT_FALSE(result.success);
    ASSERT_TRUE(result.metadata.contains("exit_code")) << result.metadata.dump();
    EXPECT_EQ(result.metadata["exit_code"].get<int>(), 5);
}

// 场景:以 [特性]param 开头的脚本(只能出现在脚本开头)。
// 期望:不拼前置脚本,照常执行。回归表现:「意外的属性 'CmdletBinding'」解析错误。
TEST_F(BashToolPowerShellEncodingTest, LeadingAttributedParamStillRuns) {
    const auto result = run("[CmdletBinding()]param($a='x')\n\"a=$a\"");
    EXPECT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find("a=x"), std::string::npos) << result.output;
}

#endif // _WIN32
