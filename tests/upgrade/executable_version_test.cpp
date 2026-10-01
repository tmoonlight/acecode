#include <gtest/gtest.h>

#include "upgrade/apply.hpp"
#include "upgrade/executable_version.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace acecode::upgrade;

namespace {
class VersionFixture {
public:
    VersionFixture() {
        root = fs::temp_directory_path() / fs::u8path(
            "acecode version \u9a8c\u8bc1 " + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "staging");
        fs::create_directories(root / "install");
#ifdef _WIN32
        name = "acecode.exe";
        target = "windows-x64";
#else
        name = "acecode";
        target = "linux-x64";
#endif
        executable = root / "staging" / name;
        fs::copy_file(fs::u8path(ACECODE_UPGRADE_VERSION_FIXTURE), executable);
        fs::permissions(executable, fs::perms::owner_exec, fs::perm_options::add);
    }
    ~VersionFixture() { std::error_code ec; fs::remove_all(root, ec); }
    void mode(const std::string& value) {
        std::ofstream(root / "staging" / ".version-probe-mode") << value;
    }
    fs::path root, executable, name;
    std::string target;
};
std::string read_text(const fs::path& path) {
    std::ifstream input(path);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}
}

TEST(UpgradeExecutableVersion, ParsesOnlyAcecodeVersionOutput) {
    EXPECT_EQ(parse_executable_version_output("acecode v0.9.14\r\n"), "0.9.14");
    EXPECT_EQ(parse_executable_version_output("acecode v0.9.14-pre.1\n"), "0.9.14-pre.1");
    EXPECT_FALSE(parse_executable_version_output("other v0.9.14\n"));
    EXPECT_FALSE(parse_executable_version_output("acecode v0.9.14\nextra"));
    EXPECT_FALSE(parse_executable_version_output("acecode vgarbage"));
}

TEST(UpgradeExecutableVersion, ChecksVersionWithoutShellWithUnicodeAndSpaces) {
    VersionFixture fixture;
    std::string error;
    EXPECT_TRUE(verify_executable_version(fixture.executable, "9.9.9", &error)) << error;
    fixture.mode("old");
    EXPECT_FALSE(verify_executable_version(fixture.executable, "9.9.9", &error));
    EXPECT_NE(error.find("expected 9.9.9, got 0.1.0"), std::string::npos);
}

TEST(UpgradeExecutableVersion, RejectsFailureMalformedOutputAndOversizedOutput) {
    VersionFixture fixture;
    for (const auto* mode : {"exit", "invalid", "flood"}) {
        fixture.mode(mode);
        std::string error;
        EXPECT_FALSE(verify_executable_version(fixture.executable, "9.9.9", &error)) << mode;
        EXPECT_FALSE(error.empty());
    }
}

// 触发场景:探测目标 sleep 10 秒不退出,调用方显式给 100ms 超时。
// 期望行为:按时判失败并只杀掉自己拉起的探测进程;错误文案写明等了多久、
// 并点出最常见的原因(安全软件还在扫描),升级日志与界面只看得到这一句。
// 3 秒上限:100ms 超时 + 终止等待,实测远小于 1 秒,3 秒只为留出机器抖动。
TEST(UpgradeExecutableVersion, TimesOutAndTerminatesOnlyItsProbe) {
    VersionFixture fixture;
    fixture.mode("timeout");
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(verify_executable_version(fixture.executable, "9.9.9", &error,
                                         std::chrono::milliseconds(100)));
    EXPECT_NE(error.find("timed out after 100ms"), std::string::npos) << error;
    EXPECT_NE(error.find("security software"), std::string::npos) << error;
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
    fixture.mode("");
    EXPECT_TRUE(verify_executable_version(fixture.executable, "9.9.9", &error)) << error;
}

// 触发场景:不传超时参数,走升级流程实际使用的默认值。
// 期望行为:默认超时至少 60 秒。
// 回归背景(反馈 ZHAOZEYIN831,0.9.26 → 0.9.27):默认值曾是 5 秒,企业杀软在新
// exe 首次执行时整包扫描,`--version` 5 秒内返回不了,下载 / 校验 / 解压都成功的
// 升级在最后一步被判失败并回滚。60 秒对应 Defender 云端首见拦截可挂起执行的上限;
// 正常机器上探测子进程一退出就返回,这个值只在真卡住时才会被用满。
TEST(UpgradeExecutableVersion, DefaultProbeTimeoutToleratesSecurityScanDelay) {
    EXPECT_GE(kExecutableVersionProbeTimeout, std::chrono::seconds(60));
}

TEST(UpgradeExecutableVersion, RejectsOldStagingWithoutChangingInstallation) {
    VersionFixture fixture;
    fixture.mode("old");
    std::ofstream(fixture.root / "install" / fixture.name) << "previous executable";
    std::string error;
    EXPECT_FALSE(apply_staged_update(fixture.root / "staging", fixture.root / "install",
        fixture.root / "backup", fixture.target, &error, nullptr, "9.9.9"));
    EXPECT_EQ(read_text(fixture.root / "install" / fixture.name), "previous executable");
    EXPECT_FALSE(fs::exists(fixture.root / "backup"));
}

TEST(UpgradeExecutableVersion, RollsBackWhenInstalledVersionVerificationFails) {
    VersionFixture fixture;
    fixture.mode("wrong-installed");
    std::ofstream(fixture.root / "install" / fixture.name) << "previous executable";
    std::string error;
    EXPECT_FALSE(apply_staged_update(fixture.root / "staging", fixture.root / "install",
        fixture.root / "backup", fixture.target, &error, nullptr, "9.9.9"));
    EXPECT_NE(error.find("version mismatch"), std::string::npos);
    EXPECT_EQ(read_text(fixture.root / "install" / fixture.name), "previous executable");
}

TEST(UpgradeExecutableVersion, CompletesOnlyWhenInstalledExecutableReportsTarget) {
    VersionFixture fixture;
    std::ofstream(fixture.root / "install" / fixture.name) << "previous executable";
    std::string error;
    EXPECT_TRUE(apply_staged_update(fixture.root / "staging", fixture.root / "install",
        fixture.root / "backup", fixture.target, &error, nullptr, "9.9.9")) << error;
    EXPECT_TRUE(verify_executable_version(fixture.root / "install" / fixture.name, "9.9.9", &error));
    EXPECT_EQ(read_text(fixture.root / "backup" / fixture.name), "previous executable");
}
