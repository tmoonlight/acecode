#include <gtest/gtest.h>

#include "platform/native_ui/open_in_explorer.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using acecode::desktop::decide_file_manager_window_action;
using acecode::desktop::FileManagerWindowAction;
using acecode::desktop::open_directory_in_file_manager;
using acecode::desktop::open_path_in_file_manager;
using acecode::desktop::OpenInExplorerTargetKind;
using acecode::desktop::validate_open_directory_request;
using acecode::desktop::validate_open_in_explorer_request;

namespace {

fs::path make_tmp_dir(const std::string& name) {
    auto base = fs::temp_directory_path() / (name + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

std::string path_string(const fs::path& path) {
    return path.string();
}

} // namespace

TEST(DesktopOpenInExplorer, RejectsEmptyAndRelativePaths) {
    EXPECT_FALSE(validate_open_directory_request("").ok);
    auto rel = validate_open_directory_request("relative/path");
    EXPECT_FALSE(rel.ok);
    EXPECT_NE(rel.error.find("absolute"), std::string::npos);
}

TEST(DesktopOpenInExplorer, RejectsMissingDirectoryAndFiles) {
    auto root = make_tmp_dir("acecode_open_explorer_missing");
    auto missing = validate_open_directory_request(path_string(root / "missing"));
    EXPECT_FALSE(missing.ok);

    auto file = root / "file.txt";
    std::ofstream(file.string()) << "x";
    auto not_dir = validate_open_directory_request(path_string(file));
    EXPECT_FALSE(not_dir.ok);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, AllowsExistingDirectoryWithoutRootRegistration) {
    auto root = make_tmp_dir("acecode_open_explorer_arbitrary_directory");
    auto nested = root / "a" / "b";
    fs::create_directories(nested);

    auto result = validate_open_directory_request(path_string(nested));
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.path, fs::weakly_canonical(nested));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, AllowsExistingFileWithoutRootRegistration) {
    auto root = make_tmp_dir("acecode_reveal_arbitrary_file");
    auto file = root / "file.txt";
    std::ofstream(file.string()) << "x";

    auto result = validate_open_in_explorer_request(path_string(file));
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.kind, OpenInExplorerTargetKind::File);
    EXPECT_EQ(result.path, fs::weakly_canonical(file));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, AllowsFileOutsideFormerAcecodeManagedRoots) {
    auto acecode_dir = make_tmp_dir("acecode_reveal_unmanaged_file");
    auto logs = acecode_dir / "logs";
    fs::create_directories(logs);

    auto log = logs / "daemon.log";
    std::ofstream(log.string()) << "log";

    auto result = validate_open_in_explorer_request(path_string(log));
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.kind, OpenInExplorerTargetKind::File);

    std::error_code ec;
    fs::remove_all(acecode_dir, ec);
}

TEST(DesktopOpenInExplorer, CanonicalizesParentSegmentsWithoutWorkspaceBoundary) {
    auto root = make_tmp_dir("acecode_open_explorer_canonical");
    auto nested = root / "nested";
    auto target = root / "target";
    fs::create_directories(nested);
    fs::create_directories(target);

    auto requested = nested / ".." / "target";
    auto result = validate_open_directory_request(path_string(requested));
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.path, fs::weakly_canonical(target));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, DoesNotInvokeLauncherForInvalidPath) {
    auto root = make_tmp_dir("acecode_open_explorer_invalid_launch");
    bool launched = false;

    auto result = open_path_in_file_manager(
        path_string(root / "missing"),
        [&](const fs::path&, OpenInExplorerTargetKind, std::string&) {
            launched = true;
            return true;
        });

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(launched);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, UsesInjectedLauncherAfterValidation) {
    auto root = make_tmp_dir("acecode_open_explorer_launch");
    fs::path launched;

    auto result = open_directory_in_file_manager(path_string(root),
        [&](const fs::path& path, std::string&) {
            launched = path;
            return true;
        });

    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(launched, fs::weakly_canonical(root));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, FileRevealPassesFileKindToInjectedLauncher) {
    auto root = make_tmp_dir("acecode_reveal_file_launch");
    auto file = root / "file.txt";
    std::ofstream(file.string()) << "x";
    fs::path launched;
    auto launched_kind = OpenInExplorerTargetKind::Directory;

    auto result = open_path_in_file_manager(
        path_string(file),
        [&](const fs::path& path,
            OpenInExplorerTargetKind kind,
            std::string&) {
            launched = path;
            launched_kind = kind;
            return true;
        });

    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(launched, fs::weakly_canonical(file));
    EXPECT_EQ(launched_kind, OpenInExplorerTargetKind::File);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, PathRevealPreservesDirectoryKind) {
    auto root = make_tmp_dir("acecode_reveal_directory_launch");
    auto launched_kind = OpenInExplorerTargetKind::File;

    auto result = open_path_in_file_manager(
        path_string(root),
        [&](const fs::path&,
            OpenInExplorerTargetKind kind,
            std::string&) {
            launched_kind = kind;
            return true;
        });

    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(launched_kind, OpenInExplorerTargetKind::Directory);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, PropagatesInjectedLauncherFailure) {
    auto root = make_tmp_dir("acecode_open_explorer_launch_fail");

    auto result = open_directory_in_file_manager(path_string(root),
        [](const fs::path&, std::string& error) {
            error = "mock failure";
            return false;
        });

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "mock failure");

    std::error_code ec;
    fs::remove_all(root, ec);
}

// 下面几条守的是「打开文件夹后资源管理器窗口要不要由我们拉到前台」。
// 句柄用整数代替 HWND:1 = ACECode 窗口,2 = 资源管理器窗口,3 = 其它程序窗口。

// 触发场景:点「在资源管理器中显示」后,资源管理器自己拿到了前台。
// 期望行为:不再动它,避免重复激活造成闪烁。
TEST(DesktopOpenInExplorer, LeavesFileManagerAloneWhenItAlreadyHasForeground) {
    EXPECT_EQ(decide_file_manager_window_action(1, 2, 2), FileManagerWindowAction::None);
}

// 触发场景:资源管理器窗口出来了,但前台仍是发起打开的 ACECode 窗口
// (Explorer 抢前台被系统拒绝,新窗口被压在 ACECode 后面)。
// 期望行为:由我们拉到前台。
// 回归:改用 SHOpenFolderAndSelectItems 之前担心的就是窗口开在后面、只在任务栏闪。
TEST(DesktopOpenInExplorer, RaisesFileManagerStuckBehindRequestingWindow) {
    EXPECT_EQ(decide_file_manager_window_action(1, 1, 2), FileManagerWindowAction::Raise);
}

// 触发场景:前台切换的瞬间 GetForegroundWindow 返回 0(没有任何窗口在前台)。
// 期望行为:当作还没人接手,照样拉起,否则这次打开会停在后面。
TEST(DesktopOpenInExplorer, RaisesFileManagerWhenForegroundIsMomentarilyEmpty) {
    EXPECT_EQ(decide_file_manager_window_action(1, 0, 2), FileManagerWindowAction::Raise);
}

// 触发场景:等窗口出现的这段时间里(目录约 0.3 秒),用户已经切到别的程序。
// 期望行为:不抢焦点,用户切走就尊重用户。
TEST(DesktopOpenInExplorer, DoesNotStealFocusAfterUserSwitchedAway) {
    EXPECT_EQ(decide_file_manager_window_action(1, 3, 2), FileManagerWindowAction::None);
}

// 触发场景:2 秒内没找到显示目标文件夹的资源管理器窗口(窗口句柄 0)。
// 期望行为:什么都不做,不能对空句柄调置前 / 置顶。
TEST(DesktopOpenInExplorer, DoesNothingWhenFileManagerWindowWasNotFound) {
    EXPECT_EQ(decide_file_manager_window_action(1, 1, 0), FileManagerWindowAction::None);
    EXPECT_EQ(decide_file_manager_window_action(1, 0, 0), FileManagerWindowAction::None);
}
