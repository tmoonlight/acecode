#include <gtest/gtest.h>

#include "gitinfo/git_context_collector.hpp"
#include "web/handlers/git_handler.hpp"
#include "worktree/worktree_manager.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace acecode;
using namespace acecode::gitinfo;

namespace {

// checkout / stash / changes / diff 的真实 git 集成测试
// (openspec add-webui-git-session-pill + redesign-sidepanel-git-changes)。
// fixture 同 tests/worktree:每用例独立临时仓库,无 git 则 SKIP。
class GitOpsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto probe = worktree::run_git({"--version"}, "");
        if (!probe.ok()) {
            GTEST_SKIP() << "git not available on this machine";
        }

        repo_ = fs::temp_directory_path() /
                ("acecode-gitops-test-" +
                 std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "-" + ::testing::UnitTest::GetInstance()
                           ->current_test_info()
                           ->name());
        std::error_code ec;
        fs::remove_all(repo_, ec);
        fs::create_directories(repo_);
        repo_utf8_ = path_to_utf8(repo_);

        ASSERT_TRUE(worktree::run_git({"init", "-b", "main"}, repo_utf8_).ok());
        ASSERT_TRUE(worktree::run_git({"config", "user.email", "t@acecode.local"},
                                      repo_utf8_).ok());
        ASSERT_TRUE(worktree::run_git({"config", "user.name", "acecode-test"},
                                      repo_utf8_).ok());
        write_file(repo_ / "README.md", "line1\nline2\nline3\n");
        ASSERT_TRUE(worktree::run_git({"add", "."}, repo_utf8_).ok());
        ASSERT_TRUE(worktree::run_git({"commit", "-m", "init"}, repo_utf8_).ok());
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(repo_, ec);
    }

    static void write_file(const fs::path& p, const std::string& content) {
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << content;
    }

    fs::path repo_;
    std::string repo_utf8_;
};

} // namespace

// ---- checkout_branch / stash_all_changes ------------------------------------

// 场景:干净工作区切到已存在的本地分支。
// 期望:成功,HEAD 指向新分支。
TEST_F(GitOpsTest, CheckoutCleanTree) {
    ASSERT_TRUE(worktree::run_git({"branch", "dev"}, repo_utf8_).ok());
    auto op = checkout_branch(repo_utf8_, "dev");
    EXPECT_TRUE(op.ok) << op.error;
    EXPECT_EQ(collect_git_info(repo_utf8_).branch, "dev");
}

// 场景:切到不存在的分支 / 非法分支名(注入)。
// 期望:失败且带错误信息;非法名在进 git argv 之前就被拦截。
TEST_F(GitOpsTest, CheckoutRejectsMissingOrUnsafeBranch) {
    auto missing = checkout_branch(repo_utf8_, "no-such-branch");
    EXPECT_FALSE(missing.ok);
    auto unsafe = checkout_branch(repo_utf8_, "$(boom)");
    EXPECT_FALSE(unsafe.ok);
    EXPECT_EQ(unsafe.error, "invalid branch name");
}

// 场景:tracked 改动 + untracked 文件一起 stash(pill 的「stash 后切换」)。
// 期望:stash 后工作区干净,untracked 文件也被收进 stash(不会被 checkout
// 丢弃 —— 这是 claude-code-haha stashToCleanState 防数据丢失的核心语义)。
TEST_F(GitOpsTest, StashAllIncludesUntracked) {
    write_file(repo_ / "README.md", "changed\n");
    write_file(repo_ / "untracked.txt", "u\n");
    auto op = stash_all_changes(repo_utf8_);
    ASSERT_TRUE(op.ok) << op.error;

    EXPECT_TRUE(list_tracked_changes(repo_utf8_).files.empty());
    EXPECT_FALSE(fs::exists(repo_ / "untracked.txt"));
    // stash pop 后两类改动都回来。
    ASSERT_TRUE(worktree::run_git({"stash", "pop"}, repo_utf8_).ok());
    EXPECT_TRUE(fs::exists(repo_ / "untracked.txt"));
}

// ---- build_git_checkout_payload(handler 纯函数)----------------------------

// 场景:workspace 有会话正在跑回合时请求 checkout。
// 期望:409 busy,git 状态零变化(busy 门在任何副作用之前)。
TEST_F(GitOpsTest, CheckoutHandlerBusyGate) {
    ASSERT_TRUE(worktree::run_git({"branch", "dev"}, repo_utf8_).ok());
    auto resp = web::build_git_checkout_payload(
        repo_utf8_, "dev", false, {repo_utf8_}, true, 3000,
        []() { return true; });
    EXPECT_EQ(resp.status, 409);
    EXPECT_EQ(resp.body["error"], "busy");
    EXPECT_EQ(collect_git_info(repo_utf8_).branch, "main");
}

// 场景:tracked 改动存在且未带 stash:true。
// 期望:409 dirty 且返回改动文件列表(前端据此弹确认框)。
TEST_F(GitOpsTest, CheckoutHandlerDirtyRoundtrip) {
    ASSERT_TRUE(worktree::run_git({"branch", "dev"}, repo_utf8_).ok());
    write_file(repo_ / "README.md", "changed\n");

    auto denied = web::build_git_checkout_payload(
        repo_utf8_, "dev", false, {repo_utf8_}, true, 3000, nullptr);
    EXPECT_EQ(denied.status, 409);
    EXPECT_EQ(denied.body["error"], "dirty");
    ASSERT_TRUE(denied.body["files"].is_array());
    EXPECT_EQ(denied.body["files"].size(), 1u);

    // 带 stash:true 重发 → stash + checkout 成功。
    auto ok = web::build_git_checkout_payload(
        repo_utf8_, "dev", true, {repo_utf8_}, true, 3000, nullptr);
    EXPECT_EQ(ok.status, 200);
    EXPECT_EQ(collect_git_info(repo_utf8_).branch, "dev");
    EXPECT_TRUE(list_tracked_changes(repo_utf8_).files.empty());
}

// 场景:白名单外的 cwd 请求 checkout。
// 期望:400,零 git 副作用。
TEST_F(GitOpsTest, CheckoutHandlerWhitelist) {
    auto resp = web::build_git_checkout_payload(
        repo_utf8_, "dev", false, {"C:/elsewhere"}, true, 3000, nullptr);
    EXPECT_EQ(resp.status, 400);
}

// ---- list_git_changes / get_file_diff ----------------------------------------

// 场景:工作区有 tracked 修改 + untracked 新文件,相对 HEAD 列变更。
// 期望:两者都出现;tracked 带 ±行数,untracked 标 A 无行数;汇总正确。
TEST_F(GitOpsTest, ChangesListAgainstHead) {
    write_file(repo_ / "README.md", "line1\nCHANGED\nline3\nline4\n");
    write_file(repo_ / "new.txt", "n\n");

    auto list = list_git_changes(repo_utf8_, "HEAD");
    ASSERT_TRUE(list.ok) << list.error_kind;
    EXPECT_EQ(list.branch, "main");
    EXPECT_EQ(list.total_count, 2u);
    EXPECT_FALSE(list.truncated);

    auto readme = std::find_if(list.files.begin(), list.files.end(),
                               [](const auto& f) { return f.path == "README.md"; });
    ASSERT_NE(readme, list.files.end());
    EXPECT_EQ(readme->status, "M");
    EXPECT_EQ(readme->additions, 2);
    EXPECT_EQ(readme->deletions, 1);

    auto added = std::find_if(list.files.begin(), list.files.end(),
                              [](const auto& f) { return f.path == "new.txt"; });
    ASSERT_NE(added, list.files.end());
    EXPECT_EQ(added->status, "A");
    EXPECT_EQ(added->additions, -1); // untracked:行数未知,前端显示 "new"

    EXPECT_EQ(list.total_additions, 2);
    EXPECT_EQ(list.total_deletions, 1);
}

// 场景:tracked / untracked 路径都含中文,Git 默认会把非 ASCII 字节写成
// C 风格八进制转义。期望:-z 边界返回原始相对路径,且该路径可直接用于 diff。
TEST_F(GitOpsTest, ChangesListPreservesNonAsciiPathsForActions) {
    const std::string tracked_path = u8"文档/待办.md";
    const std::string untracked_path = u8"草稿/新增.txt";
    write_file(repo_ / path_from_utf8(tracked_path), "before\n");
    ASSERT_TRUE(worktree::run_git({"add", "--", tracked_path}, repo_utf8_).ok());
    ASSERT_TRUE(worktree::run_git({"commit", "-m", "add unicode path"},
                                  repo_utf8_).ok());

    write_file(repo_ / path_from_utf8(tracked_path), "after\n");
    write_file(repo_ / path_from_utf8(untracked_path), "new\n");

    auto list = list_git_changes(repo_utf8_, "HEAD");
    ASSERT_TRUE(list.ok) << list.error_kind;
    auto tracked = std::find_if(list.files.begin(), list.files.end(),
                                [&](const auto& f) { return f.path == tracked_path; });
    auto untracked = std::find_if(list.files.begin(), list.files.end(),
                                  [&](const auto& f) { return f.path == untracked_path; });
    ASSERT_NE(tracked, list.files.end());
    ASSERT_NE(untracked, list.files.end());
    EXPECT_EQ(tracked->status, "M");
    EXPECT_EQ(untracked->status, "A");
    EXPECT_EQ(tracked->path.find('\\'), std::string::npos);
    EXPECT_EQ(untracked->path.find('\\'), std::string::npos);

    auto diff = get_file_diff(repo_utf8_, "HEAD", tracked->path);
    ASSERT_TRUE(diff.ok) << diff.error_kind;
    EXPECT_NE(diff.patch.find("+after"), std::string::npos);

    auto payload = web::build_git_changes_payload(
        repo_utf8_, "HEAD", {repo_utf8_}, true, 3000);
    ASSERT_EQ(payload.status, 200);
    const auto payload_file = std::find_if(
        payload.body["files"].begin(), payload.body["files"].end(),
        [&](const auto& file) { return file.value("path", "") == tracked_path; });
    EXPECT_NE(payload_file, payload.body["files"].end());
}

// 场景:中文文件被 rename。期望:name-status / numstat 的额外 old/new NUL
// 字段都消费正确,列表只保留可供预览和 diff 使用的新路径。
TEST_F(GitOpsTest, ChangesListUsesExactRenameDestination) {
    const std::string old_path = u8"资料/旧名.txt";
    const std::string new_path = u8"资料/新名.txt";
    write_file(repo_ / path_from_utf8(old_path), "same\n");
    ASSERT_TRUE(worktree::run_git({"add", "--", old_path}, repo_utf8_).ok());
    ASSERT_TRUE(worktree::run_git({"commit", "-m", "add rename source"},
                                  repo_utf8_).ok());
    ASSERT_TRUE(worktree::run_git({"mv", "--", old_path, new_path}, repo_utf8_).ok());

    auto list = list_git_changes(repo_utf8_, "HEAD");
    ASSERT_TRUE(list.ok) << list.error_kind;
    auto renamed = std::find_if(list.files.begin(), list.files.end(),
                                [&](const auto& f) { return f.path == new_path; });
    ASSERT_NE(renamed, list.files.end());
    EXPECT_EQ(renamed->status, "R");
    EXPECT_EQ(std::count_if(list.files.begin(), list.files.end(),
                            [&](const auto& f) { return f.path == old_path; }), 0);
}

// 场景:base 非法(注入字符 / 不存在的 ref)。
// 期望:invalid_base,原始字符串绝不进 git argv 当 ref 用。
TEST_F(GitOpsTest, ChangesListRejectsInvalidBase) {
    auto bad_name = list_git_changes(repo_utf8_, "$(boom)");
    EXPECT_FALSE(bad_name.ok);
    EXPECT_EQ(bad_name.error_kind, "invalid_base");
    auto missing = list_git_changes(repo_utf8_, "origin/nope");
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.error_kind, "invalid_base");
}

// 场景:单文件懒加载 diff(tracked 修改)。
// 期望:unified patch 含 ± 行;untracked 文件合成"新增文件" patch。
TEST_F(GitOpsTest, FileDiffTrackedAndUntracked) {
    write_file(repo_ / "README.md", "line1\nCHANGED\nline3\n");
    auto tracked = get_file_diff(repo_utf8_, "HEAD", "README.md");
    ASSERT_TRUE(tracked.ok) << tracked.error_kind;
    EXPECT_NE(tracked.patch.find("-line2"), std::string::npos);
    EXPECT_NE(tracked.patch.find("+CHANGED"), std::string::npos);

    write_file(repo_ / "brand-new.txt", "hello\n");
    auto untracked = get_file_diff(repo_utf8_, "HEAD", "brand-new.txt");
    ASSERT_TRUE(untracked.ok) << untracked.error_kind;
    EXPECT_NE(untracked.patch.find("+hello"), std::string::npos);
}

// 场景:handler 层的 path 越权(../ 逃出 workspace)。
// 期望:400,不触达 git。
TEST_F(GitOpsTest, FileDiffHandlerRejectsEscapingPath) {
    auto resp = web::build_git_file_diff_payload(
        repo_utf8_, "HEAD", "../outside.txt", {repo_utf8_}, true, 3000);
    EXPECT_EQ(resp.status, 400);
}

// 场景:changes handler 正常路径(json 序列化形状)。
// 期望:200,五个汇总字段 + files 数组条目形状正确。
TEST_F(GitOpsTest, ChangesHandlerPayloadShape) {
    write_file(repo_ / "README.md", "x\n");
    auto resp = web::build_git_changes_payload(
        repo_utf8_, "HEAD", {repo_utf8_}, true, 3000);
    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(resp.body["base"], "HEAD");
    EXPECT_TRUE(resp.body["files"].is_array());
    EXPECT_TRUE(resp.body.contains("total_additions"));
    EXPECT_TRUE(resp.body.contains("truncated"));
    ASSERT_EQ(resp.body["files"].size(), 1u);
    EXPECT_EQ(resp.body["files"][0]["path"], "README.md");
    EXPECT_EQ(resp.body["files"][0]["status"], "M");
}
