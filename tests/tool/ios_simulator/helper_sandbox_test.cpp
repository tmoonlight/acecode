#include <gtest/gtest.h>

#include "tool/ios_simulator/helper_sandbox.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace acecode::ios_simulator;
namespace fs = std::filesystem;

namespace {

HelperSandboxPaths sample_paths() {
    HelperSandboxPaths paths;
    paths.helper_dir = "/Applications/ACECode.app";
    paths.listable_dirs = {"/Applications/ACECode.app/Contents", "/Applications/ACECode.app"};
    paths.xcode_app = "/Users/dev/Downloads/Xcode \"beta\".app";
    paths.home = "/Users/dev";
    paths.temp_dir = "/private/var/folders/xx/T";
    paths.cache_dir = "/private/var/folders/xx/C";
    return paths;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

// 场景:辅助进程的 Seatbelt 配置。期望:禁 IP 网络、禁读 /Users 与 /Volumes 下的
// 文件内容(放行 Xcode、辅助进程包、模拟器数据与偏好文件)、只许写模拟器数据与临时目录。
TEST(IosSimulatorHelperSandbox, ProfileDeniesNetworkPersonalFilesAndStrayWrites) {
    const auto profile = helper_sandbox_profile(sample_paths());
    EXPECT_EQ(profile.rfind("(version 1)\n(allow default)\n", 0), 0u);
    EXPECT_NE(profile.find("(deny network-outbound (remote ip \"*:*\"))"), std::string::npos);
    EXPECT_NE(profile.find("(deny network-inbound (local ip \"*:*\"))"), std::string::npos);
    EXPECT_NE(profile.find("(deny network-bind (local ip \"*:*\"))"), std::string::npos);
    EXPECT_NE(profile.find("(deny file-read-data (subpath \"/Users\") (subpath \"/Volumes\") "
                           "(subpath (param \"HOME_DIR\")))"),
              std::string::npos);
    for (const char* name : {"HELPER_DIR", "XCODE_APP", "SIM_DATA", "SIM_LOGS", "CORE_DEVICE", "TEMP_DIR"})
        EXPECT_NE(profile.find(std::string("(subpath (param \"") + name + "\"))"), std::string::npos) << name;
    EXPECT_NE(profile.find("(literal (param \"PREF_SIMULATOR\"))"), std::string::npos);
    EXPECT_NE(profile.find("(literal (param \"LISTABLE_0\"))"), std::string::npos);
    EXPECT_NE(profile.find("(deny file-write* (subpath \"/\"))"), std::string::npos);
    const auto writes = profile.substr(profile.find("(allow file-write*"));
    EXPECT_NE(writes.find("(subpath \"/dev\")"), std::string::npos);
    EXPECT_NE(writes.find("SIM_DATA"), std::string::npos);
    EXPECT_EQ(writes.find("XCODE_APP"), std::string::npos) << "Xcode stays read-only";
    EXPECT_EQ(writes.find("HELPER_DIR"), std::string::npos) << "the helper bundle stays read-only";
    // 后写的规则优先:放行必须排在对应的拒绝之后。
    EXPECT_LT(profile.find("(deny file-read-data"), profile.find("(allow file-read-data"));
    EXPECT_LT(profile.find("(deny file-write*"), profile.find("(allow file-write*"));
}

// 场景:路径(含引号、空格)只经 -D 参数传入,绝不拼进 SBPL 文本。
TEST(IosSimulatorHelperSandbox, PathsTravelAsParametersNotProfileText) {
    const auto paths = sample_paths();
    const auto argv = sandboxed_helper_argv(paths, {"/Applications/ACECode.app/Contents/MacOS/acecode-ios-sim",
                                                    "--developer-dir", "/x/Contents/Developer"});
    ASSERT_GE(argv.size(), 4u);
    EXPECT_EQ(argv[0], "/usr/bin/sandbox-exec");
    EXPECT_EQ(argv[1], "-p");
    EXPECT_EQ(argv[2].find("Downloads"), std::string::npos);
    EXPECT_EQ(argv[2].find("/Users/dev"), std::string::npos);
    EXPECT_TRUE(contains(argv, "-DXCODE_APP=/Users/dev/Downloads/Xcode \"beta\".app"));
    EXPECT_TRUE(contains(argv, "-DSIM_DATA=/Users/dev/Library/Developer/CoreSimulator"));
    EXPECT_TRUE(contains(argv, "-DLISTABLE_1=/Applications/ACECode.app"));
    EXPECT_EQ(argv[argv.size() - 3], "/Applications/ACECode.app/Contents/MacOS/acecode-ios-sim");
    EXPECT_EQ(argv.back(), "/x/Contents/Developer");
    // 每个 -D 参数都在被沙盒的命令之前。
    const auto command = std::find(argv.begin(), argv.end(), argv[argv.size() - 3]);
    for (auto it = command; it != argv.end(); ++it) EXPECT_NE(it->rfind("-D", 0), 0u) << *it;
}

// 场景:缺的路径(没有 HOME、取不到缓存目录)连同用到它的规则一起省略,
// 否则 (param "X") 求值为 #f,sandbox-exec 直接拒绝整份配置。
TEST(IosSimulatorHelperSandbox, MissingPathsDropTheirRules) {
    HelperSandboxPaths paths;
    paths.helper_dir = "/opt/acecode";
    paths.xcode_app = "/Applications/Xcode.app";
    const auto profile = helper_sandbox_profile(paths);
    EXPECT_EQ(profile.find("HOME_DIR"), std::string::npos);
    EXPECT_EQ(profile.find("SIM_DATA"), std::string::npos);
    EXPECT_EQ(profile.find("CACHE_DIR"), std::string::npos);
    EXPECT_NE(profile.find("(deny file-read-data (subpath \"/Users\") (subpath \"/Volumes\"))"), std::string::npos);
    const auto argv = sandboxed_helper_argv(paths, {"helper"});
    for (const auto& arg : argv) {
        if (arg.rfind("-D", 0) != 0) continue;
        EXPECT_NE(arg.back(), '=') << "empty parameter " << arg;
    }
}

TEST(IosSimulatorHelperSandbox, XcodeAppForDeveloperDir) {
    EXPECT_EQ(xcode_app_for_developer_dir("/Applications/Xcode.app/Contents/Developer"), "/Applications/Xcode.app");
    EXPECT_EQ(xcode_app_for_developer_dir("/Users/a/Xcode-beta.app/Contents/Developer/"), "/Users/a/Xcode-beta.app");
    EXPECT_EQ(xcode_app_for_developer_dir("/Library/Developer/CommandLineTools"), "/Library/Developer/CommandLineTools");
}

#ifdef __APPLE__
// 场景:辅助进程装在 X.app/Contents/MacOS 里时整个包可读(Info.plist 在 Contents/);
// 不在包里时只放行可执行文件所在目录,祖先目录只许列名(CFBundle 定位主包要用)。
TEST(IosSimulatorHelperSandbox, CurrentPathsUseTheBundleRootAndListableAncestors) {
    const auto root = fs::temp_directory_path() / "acecode-helper-sandbox-test";
    fs::remove_all(root);
    const auto macos = root / "Apps" / "ACECode.app" / "Contents" / "MacOS";
    fs::create_directories(macos);
    std::ofstream(macos / "acecode-ios-sim") << "";
    const auto canonical_root = fs::canonical(root);

    auto paths = current_helper_sandbox_paths((macos / "acecode-ios-sim").string(),
                                              "/Applications/Xcode.app/Contents/Developer");
    EXPECT_EQ(paths.helper_dir, (canonical_root / "Apps" / "ACECode.app").string());
    ASSERT_EQ(paths.listable_dirs.size(), 2u);
    EXPECT_EQ(paths.listable_dirs[0], (canonical_root / "Apps" / "ACECode.app" / "Contents").string());
    EXPECT_EQ(paths.listable_dirs[1], (canonical_root / "Apps" / "ACECode.app").string());
    EXPECT_EQ(paths.xcode_app, fs::weakly_canonical("/Applications/Xcode.app").string());

    const auto build = root / "build" / "macos";
    fs::create_directories(build);
    paths = current_helper_sandbox_paths((build / "acecode-ios-sim").string(), "/x/Xcode.app/Contents/Developer");
    EXPECT_EQ(paths.helper_dir, (canonical_root / "build" / "macos").string());
    ASSERT_EQ(paths.listable_dirs.size(), 2u);
    EXPECT_EQ(paths.listable_dirs[0], (canonical_root / "build").string());
    EXPECT_EQ(paths.listable_dirs[1], canonical_root.string());
    fs::remove_all(root);
}

TEST(IosSimulatorHelperSandbox, EnvironmentEscapeHatchDisablesIt) {
    const char* previous = std::getenv("ACECODE_IOS_SIM_SANDBOX");
    const std::string saved = previous ? previous : "";
    setenv("ACECODE_IOS_SIM_SANDBOX", "0", 1);
    std::string reason;
    EXPECT_FALSE(helper_sandbox_enabled(&reason));
    EXPECT_NE(reason.find("ACECODE_IOS_SIM_SANDBOX=0"), std::string::npos);
    if (previous) setenv("ACECODE_IOS_SIM_SANDBOX", saved.c_str(), 1);
    else unsetenv("ACECODE_IOS_SIM_SANDBOX");
    EXPECT_TRUE(helper_sandbox_enabled());
}
#endif
