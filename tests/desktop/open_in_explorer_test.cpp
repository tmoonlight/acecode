#include <gtest/gtest.h>

#include "desktop/open_in_explorer.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

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
