#include <gtest/gtest.h>
#include "sandbox/sandbox_policy.hpp"
#include "test_support/sandbox/test_support.hpp"
#include <algorithm>

using namespace acecode;
using namespace acecode::sandbox;
namespace fs = std::filesystem;

// 场景:配置根/临时根可选,只读策略不继承任何写授权。
TEST(SandboxPolicy, ComputesRootsAndProtectsMissingSensitivePaths) {
    test::TempTree tree;
    const auto workspace = tree.dir("workspace");
    tree.dir("workspace/.git");
    const auto extra = tree.dir("extra");
    SandboxPolicyOptions options;
    options.include_tmpdir = false;
    options.extra_writable_roots = {path_to_utf8(extra), "relative/path"};
    auto policy = make_sandbox_policy(SandboxMode::WorkspaceWrite, path_to_utf8(workspace), options);
    ASSERT_EQ(policy.writable_roots.size(), 2u);
    const auto& paths = policy.writable_roots[0].read_only_subpaths;
    for (const char* child : {".git/hooks", ".git/config", ".git/config.worktree", ".git/modules", ".acecode/rules"}) {
        EXPECT_NE(std::find(paths.begin(), paths.end(), path_to_utf8(fs::weakly_canonical(workspace / child))), paths.end()) << child;
    }
    EXPECT_TRUE(make_sandbox_policy(SandboxMode::ReadOnly, path_to_utf8(workspace), options).writable_roots.empty());
    EXPECT_TRUE(policy.temporary_directory.empty());
    EXPECT_TRUE(make_sandbox_policy(SandboxMode::ReadOnly, path_to_utf8(workspace), {}).temporary_directory.empty());
    options.include_tmpdir = true;
    options.tmpdir_override = path_to_utf8(tree.dir("tmp"));
    EXPECT_EQ(compute_writable_roots(path_to_utf8(workspace), options).size(), 3u);
}

// 场景:真正登记的 linked worktree 可写其 Git 数据;伪造指针不能任意扩大根。
TEST(SandboxPolicy, ValidatesLinkedWorktreeBackReferenceAndCommonDirectory) {
    test::TempTree tree;
    auto workspace = tree.dir("work");
    auto gitdir = tree.dir("main/.git/worktrees/work");
    tree.write(workspace / ".git", "gitdir: " + path_to_utf8(gitdir));
    tree.write(gitdir / "commondir", "../..");
    tree.write(gitdir / "gitdir", path_to_utf8(workspace / ".git"));
    auto linked = resolve_linked_git_dirs(path_to_utf8(workspace));
    EXPECT_EQ(linked.gitdir, path_to_utf8(gitdir));
    EXPECT_EQ(linked.common_dir, path_to_utf8(gitdir.parent_path().parent_path()));
    tree.write(gitdir / "commondir", path_to_utf8(tree.dir("outside")));
    EXPECT_TRUE(resolve_linked_git_dirs(path_to_utf8(workspace)).gitdir.empty());
    tree.write(gitdir / "commondir", "../..");
    tree.write(gitdir / "gitdir", path_to_utf8(tree.root / "unregistered/.git"));
    EXPECT_TRUE(resolve_linked_git_dirs(path_to_utf8(workspace)).gitdir.empty());
}

// 场景:权限清单(align-codex-sandboxing D2)—— `~` / `:workspace_roots` / `:tmpdir` /
// `:acecode_home` 记号展开,deny 条目分成路径与 glob,默认 deny 名单可展开。
// 期望:记号按上下文展开,未知记号被丢弃,glob 与普通路径分流。
TEST(SandboxPolicy, ExpandsEntryTokensAndSplitsGlobs) {
    EntryContext ctx;
    ctx.home = "/home/u";
    ctx.workspace_roots = {"/work", "/main/.git/worktrees/w"};
    ctx.tmpdir = "/tmp/acecode";
    ctx.acecode_home = "/home/u/.acecode";
    EXPECT_EQ(expand_entry("~/.ssh", ctx), std::vector<std::string>{"/home/u/.ssh"});
    EXPECT_EQ(expand_entry("~", ctx), std::vector<std::string>{"/home/u"});
    EXPECT_EQ(expand_entry(":workspace_roots/node_modules", ctx),
              (std::vector<std::string>{"/work/node_modules", "/main/.git/worktrees/w/node_modules"}));
    EXPECT_EQ(expand_entry(":tmpdir", ctx), std::vector<std::string>{"/tmp/acecode"});
    EXPECT_EQ(expand_entry(":acecode_home/config.json", ctx), std::vector<std::string>{"/home/u/.acecode/config.json"});
    EXPECT_TRUE(expand_entry(":unknown_token/x", ctx).empty());
    EXPECT_TRUE(expand_entry("~other/x", ctx).empty());
    EXPECT_TRUE(is_glob_entry("/home/u/**/.env"));
    EXPECT_FALSE(is_glob_entry("/home/u/.ssh"));
    EXPECT_FALSE(default_denied_entries().empty());
}

// 场景:glob 匹配(`**` 跨目录、`*` `?` 不跨 `/`、命中祖先目录整棵生效)。
TEST(SandboxPolicy, GlobMatchingFollowsGitStyleSemantics) {
    EXPECT_TRUE(glob_matches_path("/home/u/**/.env", "/home/u/proj/.env"));
    EXPECT_TRUE(glob_matches_path("/home/u/**/.env", "/home/u/.env"));
    EXPECT_TRUE(glob_matches_path("/home/u/**/.secrets", "/home/u/a/.secrets/key.pem")) << "命中祖先目录整棵生效";
    EXPECT_FALSE(glob_matches_path("/home/u/*.key", "/home/u/sub/x.key")) << "* 不跨目录";
    EXPECT_TRUE(glob_matches_path("/home/u/*.key", "/home/u/x.key"));
    EXPECT_TRUE(glob_matches_path("/home/u/?.key", "/home/u/a.key"));
    EXPECT_FALSE(glob_matches_path("/home/u/?.key", "/home/u/ab.key"));
    EXPECT_TRUE(glob_matches_path("/home/u/[!a]*.key", "/home/u/b1.key"));
    EXPECT_FALSE(glob_matches_path("/home/u/[!a]*.key", "/home/u/a1.key"));
}

// 场景:deny 名单、可写根、只读子路径、可读根共存时的访问判定。期望:最长前缀
// 命中优先(写根下的 deny 子目录仍 Deny;deny 目录下配置的写根反而可写),同深度
// deny > write > read;全盘可读时未命中的路径 Read,受限读时 Deny;FullAccess 恒 Write。
TEST(SandboxPolicy, ResolveAccessPrefersDeepestEntryThenDenyWriteRead) {
    SandboxPolicy policy;
    policy.mode = SandboxMode::WorkspaceWrite;
    policy.writable_roots = {{"/work", {"/work/.acecode/rules"}}, {"/home/u/.cache/allowed", {}}};
    policy.denied_paths = {"/work/secrets", "/home/u/.cache"};
    policy.denied_globs = {"/work/**/.env"};
    EXPECT_EQ(policy.resolve_access("/work/src/main.cpp"), FsAccess::Write);
    EXPECT_EQ(policy.resolve_access("/work/.acecode/rules/x.rules"), FsAccess::Read);
    EXPECT_EQ(policy.resolve_access("/work/secrets/key"), FsAccess::Deny);
    EXPECT_EQ(policy.resolve_access("/work/app/.env"), FsAccess::Deny);
    EXPECT_EQ(policy.resolve_access("/home/u/.cache/other"), FsAccess::Deny);
    EXPECT_EQ(policy.resolve_access("/home/u/.cache/allowed/pkg"), FsAccess::Write) << "更深的写根压过浅的 deny";
    EXPECT_EQ(policy.resolve_access("/etc/hosts"), FsAccess::Read) << "全盘可读";
    EXPECT_TRUE(policy.can_read("/etc/hosts"));
    EXPECT_FALSE(policy.can_write("/etc/hosts"));
    policy.readable_roots = {"/work", "/usr"};
    EXPECT_EQ(policy.resolve_access("/etc/hosts"), FsAccess::Deny) << "受限读未命中";
    EXPECT_EQ(policy.resolve_access("/usr/lib/libc.so"), FsAccess::Read);
    SandboxPolicy full;
    EXPECT_EQ(full.resolve_access("/anything"), FsAccess::Write);
    // 同深度:同一路径既是写根又在 deny 里 → Deny。
    SandboxPolicy tie;
    tie.mode = SandboxMode::WorkspaceWrite;
    tie.writable_roots = {{"/both", {}}};
    tie.denied_paths = {"/both"};
    EXPECT_EQ(tie.resolve_access("/both/x"), FsAccess::Deny);
}

// 场景:make_sandbox_policy 把配置条目、默认 deny 名单、会话授权揉进策略。期望:
// deny 展开成绝对路径 / glob;落在可写根下的 deny 路径同时列为只读子路径(三平台
// 写拒绝一致);授权的写路径进可写根,但在 deny 名单上的授权被忽略;配置了 read
// 条目才启用受限读,且可写根与工作区根隐含可读;ReadOnly 模式也带 deny。
TEST(SandboxPolicy, EntriesAndGrantsFlowIntoPolicy) {
    test::TempTree tree;
    const auto workspace = tree.dir("work");
    const auto home = tree.dir("home");
    tree.dir("home/.ssh");
    const auto cache = tree.dir("home/.cache/pnpm");
    tree.dir("work/private");
    SandboxPolicyOptions options;
    options.include_tmpdir = false;
    options.home_override = path_to_utf8(home);
    options.denied_entries = {"~/.ssh", ":workspace_roots/private", "~/**/.env", "relative/ignored"};
    options.grants.write = {path_to_utf8(cache), path_to_utf8(home / ".ssh")};
    auto policy = make_sandbox_policy(SandboxMode::WorkspaceWrite, path_to_utf8(workspace), options);
    const std::string ssh = path_to_utf8(fs::weakly_canonical(home / ".ssh"));
    const std::string private_dir = path_to_utf8(fs::weakly_canonical(workspace / "private"));
    EXPECT_NE(std::find(policy.denied_paths.begin(), policy.denied_paths.end(), ssh), policy.denied_paths.end());
    EXPECT_NE(std::find(policy.denied_paths.begin(), policy.denied_paths.end(), private_dir), policy.denied_paths.end());
    ASSERT_EQ(policy.denied_globs.size(), 1u);
    EXPECT_NE(policy.denied_globs[0].find("/**/.env"), std::string::npos);
    ASSERT_FALSE(policy.writable_roots.empty());
    const auto& ro = policy.writable_roots[0].read_only_subpaths;
    EXPECT_NE(std::find(ro.begin(), ro.end(), private_dir), ro.end()) << "写根下的 deny 路径列为只读子路径";
    bool cache_root = false, ssh_root = false;
    for (const auto& root : policy.writable_roots) {
        if (root.root == path_to_utf8(fs::weakly_canonical(cache))) cache_root = true;
        if (root.root == ssh) ssh_root = true;
    }
    EXPECT_TRUE(cache_root) << "会话授权的写路径进可写根";
    EXPECT_FALSE(ssh_root) << "deny 名单压过授权";
    EXPECT_TRUE(policy.full_disk_read());
    EXPECT_EQ(policy.resolve_access(ssh + "/id_rsa"), FsAccess::Deny);
    EXPECT_EQ(policy.resolve_access(path_to_utf8(home / "app" / ".env")), FsAccess::Deny) << "`~/**/.env` 命中家目录下任意层级";
    EXPECT_EQ(policy.resolve_access(path_to_utf8(workspace / "app" / ".env")), FsAccess::Write) << "glob 锚定在家目录,工作区不受影响";
    // 受限读:可写根与工作区根隐含可读。
    options.readable_roots = {"~/.cache"};
    auto restricted = make_sandbox_policy(SandboxMode::ReadOnly, path_to_utf8(workspace), options);
    EXPECT_FALSE(restricted.full_disk_read());
    EXPECT_EQ(restricted.resolve_access(path_to_utf8(workspace / "src")), FsAccess::Read);
    EXPECT_EQ(restricted.resolve_access(path_to_utf8(tree.root / "elsewhere")), FsAccess::Deny);
    EXPECT_FALSE(restricted.denied_paths.empty()) << "ReadOnly 模式也带 deny";
    EXPECT_NE(describe_policy(restricted, true).find("readable:"), std::string::npos);
    EXPECT_NE(describe_policy(policy, false, true).find("best-effort offline"), std::string::npos);
}

// 场景:AdditionalPermissions 的合并与覆盖判定。期望:merge 去重;covers 按前缀
// 判(已授权 /a 覆盖申请 /a/b);写授权也覆盖读申请;网络申请需要网络授权。
TEST(SandboxPolicy, AdditionalPermissionsMergeAndCover) {
    AdditionalPermissions granted;
    granted.write = {"/a"};
    granted.read = {"/r"};
    AdditionalPermissions more;
    more.write = {"/a", "/b"};
    granted.merge(more);
    EXPECT_EQ(granted.write, (std::vector<std::string>{"/a", "/b"}));
    AdditionalPermissions ask;
    ask.write = {"/a/sub"};
    ask.read = {"/a/other", "/r/x"};
    EXPECT_TRUE(granted.covers(ask));
    ask.network = true;
    EXPECT_FALSE(granted.covers(ask));
    granted.network = true;
    EXPECT_TRUE(granted.covers(ask));
    ask.write.push_back("/c");
    EXPECT_FALSE(granted.covers(ask));
    EXPECT_TRUE(AdditionalPermissions{}.empty());
}

// 场景:用户把 `.git/hooks` 本身配成额外可写根。期望:它仍出现在自己的只读
// 子路径里 —— 受保护子路径不能靠再声明一个重叠的可写根解开。
TEST(SandboxPolicy, OverlappingRootCannotReenableProtectedSubdirectory) {
    test::TempTree tree;
    auto workspace = tree.dir("work");
    auto hooks = tree.dir("work/.git/hooks");
    SandboxPolicyOptions options;
    options.include_tmpdir = false;
    options.extra_writable_roots = {path_to_utf8(hooks)};
    auto roots = compute_writable_roots(path_to_utf8(workspace), options);
    ASSERT_EQ(roots.size(), 2u);
    EXPECT_NE(std::find(roots[1].read_only_subpaths.begin(), roots[1].read_only_subpaths.end(),
                        path_to_utf8(hooks)), roots[1].read_only_subpaths.end());
}
