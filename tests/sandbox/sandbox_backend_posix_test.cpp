#include <gtest/gtest.h>
#include "sandbox/sandbox_backend.hpp"
#include "test_support/sandbox/test_support.hpp"
#include <algorithm>
#include <fstream>
#include <iterator>

using namespace acecode;
using namespace acecode::sandbox;

namespace {
bool has_pair(const std::vector<std::pair<std::string, std::string>>& env, const std::string& key,
              const std::string& value) {
    return std::find(env.begin(), env.end(), std::make_pair(key, value)) != env.end();
}
bool has_key(const std::vector<std::pair<std::string, std::string>>& env, const std::string& key) {
    return std::any_of(env.begin(), env.end(), [&](const auto& kv) { return kv.first == key; });
}
}

// 场景:可写根路径里含引号和 SBPL 语法(`"(allow default)`)。期望:路径只经
// `-D` 参数注入,policy 文本本身不内嵌路径(否则路径能注入策略),只读子路径
// 走 require-not,network_access=false 时没有 network-outbound。
TEST(SandboxBackendPosix, SeatbeltUsesParametersForUntrustedPathText) {
    SandboxPolicy policy;
    policy.mode = SandboxMode::WorkspaceWrite;
    policy.writable_roots = {{"/tmp/a \"(allow default)", {"/tmp/a \"(allow default)/.acecode/rules"}}};
    auto text = build_seatbelt_policy(policy);
    EXPECT_EQ(text.find(policy.writable_roots[0].root), std::string::npos);
    EXPECT_NE(text.find("require-not"), std::string::npos);
    EXPECT_EQ(text.find("(allow network-outbound)"), std::string::npos);
    auto argv = build_seatbelt_argv(policy);
    EXPECT_EQ(argv.front(), "/usr/bin/sandbox-exec");
    EXPECT_EQ(argv.back(), "--");
    EXPECT_NE(std::find(argv.begin(), argv.end(), "-DWRITABLE_ROOT_0=" + policy.writable_roots[0].root), argv.end());
}

// 场景:全盘可读 + deny 路径 + deny glob 的 Seatbelt 策略(align-codex-sandboxing D8)。
// 期望:全盘 `(allow file-read*)` 与 preferences policy 都在;deny 路径经 `-D` 参数
// 变成 `deny file-read* file-write*`,且排在 allow 之后(后出现的规则优先);
// glob 变成正则 deny 并对祖先目录加 file-write-unlink;每个可写根本身也有
// unlink 拒绝(改名边界目录 = 把 carveout 搬走)。
TEST(SandboxBackendPosix, SeatbeltDeniesSecretPathsAndGlobsAfterAllows) {
    SandboxPolicy policy;
    policy.mode = SandboxMode::WorkspaceWrite;
    policy.writable_roots = {{"/work", {"/work/.acecode/rules"}}};
    policy.denied_paths = {"/Users/u/.ssh"};
    policy.denied_globs = {"/Users/u/**/.env"};
    const auto text = build_seatbelt_policy(policy);
    EXPECT_NE(text.find("(allow file-read*)"), std::string::npos);
    EXPECT_NE(text.find("user-preference-read"), std::string::npos);
    const auto allow_write = text.find("(allow file-write*");
    const auto deny_path = text.find("(deny file-read* file-write* (subpath (param \"DENIED_PATH_0\")))");
    ASSERT_NE(deny_path, std::string::npos);
    EXPECT_LT(allow_write, deny_path);
    EXPECT_EQ(text.find("/Users/u/.ssh"), std::string::npos) << "deny 路径不能内嵌进策略文本";
    EXPECT_NE(text.find("(deny file-read* (regex #\"^/Users/u/(.*/)?\\.env$\"))"), std::string::npos) << text;
    EXPECT_NE(text.find("(deny file-write-unlink (require-all (vnode-type DIRECTORY) (regex #\"^/Users/u$\")))"), std::string::npos);
    EXPECT_NE(text.find("(regex #\"^/Users$\")"), std::string::npos);
    // 有意偏离 Codex:`**` 之下的目录不禁止改名,否则家目录下任何目录都删不掉。
    EXPECT_EQ(text.find("(regex #\"^/Users/u/.*$\")"), std::string::npos);
    EXPECT_NE(text.find("(deny file-write-unlink (require-all (literal (param \"WRITABLE_ROOT_0\")) (vnode-type DIRECTORY)))"), std::string::npos);
    // 只读子路径 /work/.acecode/rules 的祖先 /work/.acecode 不能被改名。
    EXPECT_NE(text.find("PROTECTED_ANCESTOR_0"), std::string::npos);
    const auto argv = build_seatbelt_argv(policy);
    EXPECT_NE(std::find(argv.begin(), argv.end(), "-DDENIED_PATH_0=/Users/u/.ssh"), argv.end());
    EXPECT_NE(std::find(argv.begin(), argv.end(), "-DPROTECTED_ANCESTOR_0=/work/.acecode"), argv.end());
}

// 场景:受限读(配置了 read 条目)。期望:不再有全盘 `(allow file-read*)`,可读根
// 经 READABLE_ROOT_i 参数放行,追加平台默认项(系统框架 / /etc / 终端设备),
// 不追加 preferences policy(cfprefsd 会把可读根之外的数据透出来)。
TEST(SandboxBackendPosix, SeatbeltRestrictedReadUsesRootsAndPlatformDefaults) {
    SandboxPolicy policy;
    policy.mode = SandboxMode::ReadOnly;
    policy.readable_roots = {"/work", "/opt/toolchain"};
    const auto text = build_seatbelt_policy(policy);
    EXPECT_EQ(text.find("\n(allow file-read*)\n"), std::string::npos);
    EXPECT_NE(text.find("(allow file-read* (subpath (param \"READABLE_ROOT_1\")))"), std::string::npos);
    EXPECT_NE(text.find("/System/Library/Frameworks"), std::string::npos);
    EXPECT_EQ(text.find("user-preference-read"), std::string::npos);
    const auto argv = build_seatbelt_argv(policy);
    EXPECT_NE(std::find(argv.begin(), argv.end(), "-DREADABLE_ROOT_0=/work"), argv.end());
    EXPECT_NE(std::find(argv.begin(), argv.end(), "-DREADABLE_ROOT_1=/opt/toolchain"), argv.end());
}

// 场景:glob → Seatbelt 正则的转换表。期望:`*` `?` 不跨 `/`,`**/` 可为空,
// 字面量在 subtree 模式下带 `(/.*)?`、exact 模式不带,元字符转义,花括号成交替。
TEST(SandboxBackendPosix, GlobToSeatbeltRegexTable) {
    // 与 Codex seatbelt_regex_for_glob 一致:含 glob 元字符的模式不追加 `(/.*)?`,
    // 只有纯字面量在 subtree 模式下才匹配子树。
    EXPECT_EQ(seatbelt_regex_for_glob("/a/**/.env", true), "^/a/(.*/)?\\.env$");
    EXPECT_EQ(seatbelt_regex_for_glob("/a/*.key", true), "^/a/[^/]*\\.key$");
    EXPECT_EQ(seatbelt_regex_for_glob("/a/?", true), "^/a/[^/]$");
    EXPECT_EQ(seatbelt_regex_for_glob("/a/b", true), "^/a/b(/.*)?$");
    EXPECT_EQ(seatbelt_regex_for_glob("/a/b", false), "^/a/b$");
    EXPECT_EQ(seatbelt_regex_for_glob("/a/{x,y}", true), "^/a/(x|y)$");
    EXPECT_EQ(seatbelt_regex_for_glob("/a/[!0-9]x", true), "^/a/[^0-9]x$");
    EXPECT_TRUE(seatbelt_regex_for_glob("", true).empty());
}

// 场景:bwrap 参数组装。期望:只读子路径的 --ro-bind 排在可写根 --bind 之后
// (后绑定覆盖先绑定);默认 --unshare-net,放行网络时去掉;用户 / PID / IPC
// 命名空间、--new-session 与 --cap-drop ALL 恒在;不存在的只读子路径跳过
// (bwrap 对缺失源路径直接报错);deny 目录用 tmpfs 遮住、deny 文件用 /dev/null。
TEST(SandboxBackendPosix, BwrapMountsProtectedPathsAfterWritableRootsAndMasksDenies) {
    test::TempTree tree;
    const auto work = tree.dir("work space");
    tree.write(work / ".git/config", "[core]");
    const auto secret_dir = tree.dir("secrets");
    const auto secret_file = tree.root / "token.txt";
    tree.write(secret_file, "x");
    SandboxPolicy policy;
    policy.mode = SandboxMode::WorkspaceWrite;
    policy.writable_roots = {{path_to_utf8(work), {path_to_utf8(work / ".git/config"), path_to_utf8(work / "missing")}}};
    policy.denied_paths = {path_to_utf8(secret_dir), path_to_utf8(secret_file), path_to_utf8(tree.root / "absent")};
    auto argv = build_bwrap_argv(policy);
    auto allow = std::find(argv.begin(), argv.end(), "--bind");
    ASSERT_NE(allow, argv.end());
    auto deny = std::find(allow, argv.end(), "--ro-bind");
    ASSERT_NE(deny, argv.end());
    EXPECT_EQ(*(allow + 1), path_to_utf8(work));
    EXPECT_EQ(*(deny + 1), path_to_utf8(work / ".git/config"));
    EXPECT_EQ(std::find(argv.begin(), argv.end(), path_to_utf8(work / "missing")), argv.end());
    auto tmpfs = std::find(argv.begin(), argv.end(), "--tmpfs");
    ASSERT_NE(tmpfs, argv.end());
    EXPECT_EQ(*(tmpfs + 1), path_to_utf8(secret_dir));
    auto null_bind = std::find(argv.begin(), argv.end(), "/dev/null");
    ASSERT_NE(null_bind, argv.end());
    EXPECT_EQ(*(null_bind + 1), path_to_utf8(secret_file));
    EXPECT_EQ(std::find(argv.begin(), argv.end(), path_to_utf8(tree.root / "absent")), argv.end());
    EXPECT_NE(std::find(argv.begin(), argv.end(), "--unshare-net"), argv.end());
    for (const auto* option : {"--unshare-user", "--unshare-pid", "--unshare-ipc", "--new-session", "--cap-drop"}) {
        EXPECT_NE(std::find(argv.begin(), argv.end(), option), argv.end());
    }
    EXPECT_EQ(argv.back(), "--");
    policy.network_access = true;
    argv = build_bwrap_argv(policy);
    EXPECT_EQ(std::find(argv.begin(), argv.end(), "--unshare-net"), argv.end());
}

// 场景:Windows 受限令牌后端的子进程环境(align-codex-sandboxing D7)。期望:
// network_access=false 时套用准断网环境(代理指向死端口、pip/npm/cargo 离线、
// ssh 失败、PATH 前插桩目录、PATHEXT 把 .BAT/.CMD 提前);network_access=true
// 或真能断网的后端(bwrap)不套用;放行网络时只剩 ACECODE_SANDBOX(+ 临时目录)。
TEST(SandboxBackendPosix, WindowsOfflineEnvironmentOnlyWhenNetworkIsNotAllowed) {
    SandboxPolicy policy;
    policy.mode = SandboxMode::WorkspaceWrite;
    const auto offline = sandbox_environment(BackendKind::WindowsRestrictedToken, policy, false,
        "C:/data/sandbox/denybin", "C:/Windows;C:/tools", ".COM;.EXE;.BAT;.CMD");
    EXPECT_TRUE(has_pair(offline, "ACECODE_SANDBOX", "restricted-token"));
    EXPECT_TRUE(has_pair(offline, "ACECODE_SANDBOX_NETWORK_DISABLED", "1"));
    EXPECT_TRUE(has_pair(offline, "HTTPS_PROXY", "http://127.0.0.1:9"));
    EXPECT_TRUE(has_pair(offline, "NO_PROXY", "localhost,127.0.0.1,::1"));
    EXPECT_TRUE(has_pair(offline, "PIP_NO_INDEX", "1"));
    EXPECT_TRUE(has_pair(offline, "NPM_CONFIG_OFFLINE", "true"));
    EXPECT_TRUE(has_pair(offline, "CARGO_NET_OFFLINE", "true"));
    EXPECT_TRUE(has_pair(offline, "GIT_SSH_COMMAND", "cmd /c exit 1"));
    EXPECT_TRUE(has_pair(offline, "PATH", "C:/data/sandbox/denybin;C:/Windows;C:/tools"));
    EXPECT_TRUE(has_pair(offline, "PATHEXT", ".BAT;.CMD;.COM;.EXE"));
    // 没有桩目录就不动 PATH / PATHEXT,其余劝退变量照旧。
    const auto no_stubs = sandbox_environment(BackendKind::WindowsRestrictedToken, policy, false);
    EXPECT_FALSE(has_key(no_stubs, "PATH"));
    EXPECT_TRUE(has_key(no_stubs, "HTTP_PROXY"));
    policy.network_access = true;
    const auto online = sandbox_environment(BackendKind::WindowsRestrictedToken, policy, false, "C:/x");
    EXPECT_EQ(online.size(), 1u);
    EXPECT_FALSE(has_key(online, "HTTP_PROXY"));
    policy.network_access = false;
    const auto bwrap = sandbox_environment(BackendKind::LinuxBwrap, policy, true);
    EXPECT_TRUE(has_pair(bwrap, "ACECODE_SANDBOX_NETWORK_DISABLED", "1"));
    EXPECT_FALSE(has_key(bwrap, "HTTP_PROXY"));
}

// 场景:准断网桩目录。期望:ssh / scp 的 .cmd 与 .bat 桩都创建,内容是失败退出;
// 重复调用不重写;目录路径为空时报错。
TEST(SandboxBackendPosix, DenybinStubsAreCreatedOnce) {
    test::TempTree tree;
    const auto dir = path_to_utf8(tree.root / "denybin");
    std::string error;
    ASSERT_TRUE(ensure_denybin_stubs(dir, &error)) << error;
    for (const char* name : {"ssh.cmd", "ssh.bat", "scp.cmd", "scp.bat"}) {
        EXPECT_TRUE(std::filesystem::exists(tree.root / "denybin" / name)) << name;
    }
    std::ifstream in(tree.root / "denybin" / "ssh.cmd", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("exit /b 1"), std::string::npos);
    EXPECT_TRUE(ensure_denybin_stubs(dir, &error));
    EXPECT_FALSE(ensure_denybin_stubs("", &error));
}

// 临时目录覆盖只用于 Windows 的 WorkspaceWrite;只读/完整访问和未追加
// 临时根(exclude_tmpdir)不能误带写目录或覆盖宿主环境。
TEST(SandboxBackendPosix, TemporaryEnvironmentOverridesAreScopedToWindowsWorkspaceWrite) {
    SandboxPolicy policy;
    policy.mode = SandboxMode::WorkspaceWrite;
    policy.network_access = true;   // 关掉准断网变量,只看临时目录覆盖。
    policy.temporary_directory = "C:/temp/acecode-sandbox/workspace";
    const auto env = sandbox_environment(BackendKind::WindowsRestrictedToken, policy, false);
    for (const char* key : {"TEMP", "TMP", "TMPDIR"}) {
        EXPECT_TRUE(has_pair(env, key, policy.temporary_directory)) << key;
    }
    EXPECT_EQ(sandbox_environment(BackendKind::LinuxBwrap, policy, false).size(), 1u);
    for (const auto mode : {SandboxMode::ReadOnly, SandboxMode::FullAccess}) {
        policy.mode = mode;
        EXPECT_EQ(sandbox_environment(BackendKind::WindowsRestrictedToken, policy, false).size(), 1u);
    }
    policy.mode = SandboxMode::WorkspaceWrite;
    policy.temporary_directory.clear();
    EXPECT_EQ(sandbox_environment(BackendKind::WindowsRestrictedToken, policy, false).size(), 1u);
}
