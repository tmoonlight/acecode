#include <gtest/gtest.h>
#include "sandbox/sandbox_denial.hpp"
#include "sandbox/sandbox_policy.hpp"
#include "test_support/sandbox/test_support.hpp"
#include "utils/utf8_path.hpp"
#include <csignal>

using namespace acecode::sandbox;

// 场景:沙盒内命令失败,输出含中英文权限拒绝特征 / 普通编译错误 / 127 / 2。
// 期望:只有非零退出码 + 特征词才判为沙盒拒绝;退出 0、用法错误(2)、
// 找不到命令(127)都不归咎沙盒;POSIX 上 seccomp 的 128+SIGSYS 直接判定。
TEST(SandboxDenial, RecognizesPermissionFailuresWithoutBlamingEveryError) {
    for (const char* output : {"Access is denied.", "拒绝访问。", "Permission denied", "Read-only file system"}) {
        EXPECT_TRUE(is_likely_sandbox_denied(1, output));
        EXPECT_FALSE(is_likely_sandbox_denied(0, output));
    }
    EXPECT_FALSE(is_likely_sandbox_denied(1, "compilation failed"));
    // 回归:输出只是提到了名为 sandbox 的源文件(本仓库 src/sandbox/ 编译失败
    // 就是这样),不能因为出现 "sandbox" 字样就判成沙盒拒绝。
    EXPECT_FALSE(is_likely_sandbox_denied(1,
        "src/sandbox/sandbox_runtime.cpp(12): error C2065: undeclared identifier"));
    EXPECT_TRUE(is_likely_sandbox_denied(1, "sandbox-exec: failed to open file"));
    EXPECT_FALSE(is_likely_sandbox_denied(127, "command not found"));
    EXPECT_FALSE(is_likely_sandbox_denied(2, "invalid option"));
#ifndef _WIN32
    EXPECT_TRUE(is_likely_sandbox_denied(128 + SIGSYS, ""));
#endif
}

// 场景:拒绝分类(align-codex-sandboxing D4,移植 Codex violation.rs)。期望:
// 原因归一成机器可读值,片段截到 512 字符,退出 0 / 2 / 126 / 127 不分类。
TEST(SandboxDenial, ClassifiesReasonAndTruncatesSnippet) {
    auto v = classify_sandbox_violation(1, "touch: cannot touch '/etc/x': Read-only file system");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->reason, "read_only_file_system");
    EXPECT_EQ(v->path, "/etc/x");
    EXPECT_EQ(v->snippet, "touch: cannot touch '/etc/x': Read-only file system");
    v = classify_sandbox_violation(1, "bwrap: Can't mkdir: Operation not permitted");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->reason, "operation_not_permitted");
    const std::string longer(2000, 'x');
    v = classify_sandbox_violation(1, "Access is denied. " + longer);
    ASSERT_TRUE(v);
    EXPECT_EQ(v->reason, "access_denied");
    EXPECT_EQ(v->snippet.size(), 512u);
    EXPECT_FALSE(classify_sandbox_violation(126, "Permission denied"));
    EXPECT_FALSE(classify_sandbox_violation(0, "Permission denied"));
}

// 场景:从三平台的报错文本里抽被拒路径。期望:POSIX `path: Permission denied`、
// 带工具名前缀的 `sh: /x: Permission denied`、PowerShell `Access to the path 'X' is
// denied`、cmd `Access is denied.` 前面的盘符路径都能抽出;没有路径的行返回空;
// 相对片段(`./x`)保留原样。
TEST(SandboxDenial, ExtractsDeniedPathAcrossPlatforms) {
    EXPECT_EQ(extract_denied_path("mkdir: /Users/u/.cache/x: Permission denied"), "/Users/u/.cache/x");
    EXPECT_EQ(extract_denied_path("sh: /work/out.txt: Permission denied"), "/work/out.txt");
    EXPECT_EQ(extract_denied_path("Out-File : Access to the path 'C:\\Users\\u\\x.txt' is denied."), "C:\\Users\\u\\x.txt");
    EXPECT_EQ(extract_denied_path("C:\\outside\\denied.txt Access is denied."), "C:\\outside\\denied.txt");
    EXPECT_EQ(extract_denied_path("open ./build/x: permission denied"), "./build/x");
    EXPECT_EQ(extract_denied_path("Permission denied"), "");
    EXPECT_EQ(extract_denied_path("everything is fine"), "");
}

// 场景:给「只放行该目录」算建议目录。期望:被拒路径是文件 → 父目录;已经可写的
// 目录不建议(不是被拒原因);deny 名单里的目录 / 命中 deny glob 的目录不建议;
// 相对路径不建议。
// 条目按 make_sandbox_policy 的约定先规范化(canonical_policy_path):Windows 上 `/work`
// 按当前盘解析,当前盘有 \work 目录时(本机 C:\work)候选目录会变成 C:\work。
// 回归:条目曾直接写 `/work`,在存在 C:\work 的机器上「已可写」判不出来,建议成了 "C:\work"。
TEST(SandboxDenial, SuggestedWriteRootRespectsPolicyAndDenyList) {
    SandboxPolicy policy;
    policy.mode = SandboxMode::WorkspaceWrite;
    policy.writable_roots = {{canonical_policy_path("/work"), {}}};
    policy.denied_paths = {canonical_policy_path("/home/u/.ssh")};
    policy.denied_globs = {canonical_policy_path("/home/u") + "/**/.secrets"};
    SandboxViolation v;
    v.path = "/home/u/.cache/pnpm/store.lock";
    EXPECT_EQ(suggested_write_root(v, policy), "/home/u/.cache/pnpm");
    v.path = "/work/out.txt";
    EXPECT_EQ(suggested_write_root(v, policy), "") << "已可写的目录不是被拒原因";
    v.path = "/home/u/.ssh/id_rsa";
    EXPECT_EQ(suggested_write_root(v, policy), "");
    v.path = "/home/u/proj/.secrets/token";
    EXPECT_EQ(suggested_write_root(v, policy), "");
    v.path = "./relative";
    EXPECT_EQ(suggested_write_root(v, policy), "");
    v.path.clear();
    EXPECT_EQ(suggested_write_root(v, policy), "");
}

// 场景:命令报 <别名>/.ssh/newdir/key 被拒,其中别名是指向真实家目录的 junction /
// 符号链接(本机 C:\Users\shao → N:\Users\shao),newdir 还不存在;deny 名单里的 ~/.ssh
// 是 canonical 的真实路径。另有一个经别名写、但不在 deny 名单里的普通目录。
// 期望:.ssh 下的目录不给「只放行该目录」建议;普通目录照常建议(修复不能误伤)。
// 回归:建议逻辑拿词法形态的候选目录和 canonical 的 deny 条目做 lexically_relative,别名
// 形态对不上,越权确认框里出现「只放行 <别名>/.ssh/newdir」的选项。
TEST(SandboxDenial, SuggestedWriteRootRejectsDeniedDirectoryReachedThroughAlias) {
    acecode::sandbox::test::TempTree tree;
    const auto real_home = tree.dir("real-home");
    tree.dir("real-home/.ssh");
    const auto workspace = tree.dir("work");
    const auto alias_home = tree.root / "alias-home";
    if (!acecode::sandbox::test::make_directory_alias(real_home, alias_home)) {
        GTEST_SKIP() << "cannot create a directory alias here";
    }
    SandboxPolicyOptions options;
    options.include_tmpdir = false;
    options.home_override = acecode::path_to_utf8(alias_home);
    options.denied_entries = {"~/.ssh"};
    const auto policy = make_sandbox_policy(
        SandboxMode::WorkspaceWrite, acecode::path_to_utf8(workspace), options);

    SandboxViolation v;
    v.path = acecode::path_to_utf8(alias_home / ".ssh" / "newdir" / "key");
    EXPECT_EQ(suggested_write_root(v, policy), "");
    v.path = acecode::path_to_utf8(alias_home / ".ssh" / "key");
    EXPECT_EQ(suggested_write_root(v, policy), "");
    v.path = acecode::path_to_utf8(alias_home / "projects" / "out.txt");
    EXPECT_NE(suggested_write_root(v, policy), "") << "deny 名单之外的普通目录照常建议";
}

// 场景:升级提示文案。期望:后端不拦网络且不做准断网(旧 Windows 语义)时不提网络;
// 拦网络时说"blocked";准断网时说"discouraged";带被拒路径时先建议
// with_additional_permissions 只加该目录,再提 require_escalated;deny 名单存在时
// 说明秘密目录不能申请。
TEST(SandboxDenial, HintSuggestsSmallestRequestFirst) {
    SandboxPolicy policy;
    policy.mode = SandboxMode::WorkspaceWrite;
    policy.writable_roots = {{"/work", {"/work/.acecode/rules"}}};
    const auto windows = escalation_hint(policy, false);
    EXPECT_EQ(windows.find("Network access is blocked"), std::string::npos);
    EXPECT_EQ(windows.find("discouraged"), std::string::npos);
    EXPECT_NE(windows.find("require_escalated"), std::string::npos);
    EXPECT_NE(windows.find("with_additional_permissions"), std::string::npos);
    EXPECT_NE(windows.find("justification"), std::string::npos);
    EXPECT_NE(escalation_hint(policy, true).find("Network access is blocked"), std::string::npos);
    EXPECT_NE(escalation_hint(policy, false, nullptr, true).find("discouraged"), std::string::npos);
    SandboxViolation v;
    v.reason = "permission_denied";
    v.path = "/home/u/.cache/pnpm/store.lock";
    const auto with_path = escalation_hint(policy, true, &v);
    EXPECT_NE(with_path.find("on path: /home/u/.cache/pnpm/store.lock"), std::string::npos);
    EXPECT_NE(with_path.find("\"write\":[\"/home/u/.cache/pnpm\"]"), std::string::npos);
    EXPECT_LT(with_path.find("with_additional_permissions"), with_path.find("require_escalated"));
    policy.denied_paths = {"/home/u/.ssh"};
    EXPECT_NE(escalation_hint(policy, true).find("cannot be requested"), std::string::npos);
    const auto json = violation_to_json(v);
    EXPECT_EQ(json["reason"], "permission_denied");
    EXPECT_EQ(json["path"], v.path);
}
