#include <gtest/gtest.h>

#include "tui/chat_file_link.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

using acecode::desktop::OpenInExplorerTargetKind;
using acecode::tui::open_tui_chat_file_link;
using acecode::tui::resolve_tui_chat_file_link;

class TempDir {
public:
    explicit TempDir(const std::string& name) {
        static std::atomic<unsigned int> sequence{0};
        path_ = fs::temp_directory_path() /
            (name + "_" +
             std::to_string(
                 ::testing::UnitTest::GetInstance()->random_seed()) +
             "_" + std::to_string(sequence.fetch_add(1)));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string utf8(const fs::path& path) {
    return acecode::path_to_utf8(path);
}

void write_file(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "test";
}

TEST(TuiChatFileLink, ExternalAndUnsupportedLinksAreNotHandled) {
    TempDir temp("acecode_tui_link_external");
    for (const std::string href : {
             "https://example.com/file.cpp",
             "http://example.com",
             "mailto:test@example.com",
             "#section",
             "//cdn.example.com/file.cpp",
             "javascript:alert(1)",
             "file:///tmp/file.cpp",
         }) {
        const auto result =
            resolve_tui_chat_file_link(href, utf8(temp.path()));
        EXPECT_FALSE(result.handled) << href;
        EXPECT_FALSE(result.ok) << href;
    }
}

TEST(TuiChatFileLink, ResolvesAbsolutePathAndStripsSourceLocation) {
    TempDir temp("acecode_tui_link_absolute");
    const fs::path file = temp.path() / "source.cpp";
    write_file(file);

    const auto result = resolve_tui_chat_file_link(
        utf8(file) + ":42:7",
        utf8(temp.path()));

    ASSERT_TRUE(result.handled);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(fs::equivalent(result.path, file));
}

TEST(TuiChatFileLink, ResolvesRelativePathFromActiveCwd) {
    TempDir temp("acecode_tui_link_relative");
    const fs::path file = temp.path() / "docs" / "guide.md";
    write_file(file);

    const auto result = resolve_tui_chat_file_link(
        "docs/guide.md:12",
        utf8(temp.path()));

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(fs::equivalent(result.path, file));
}

TEST(TuiChatFileLink, ResolvesRepositoryRelativePathFromNearestAncestor) {
    TempDir temp("acecode_tui_link_ancestor");
    const fs::path build = temp.path() / "build";
    const fs::path file = build / "acecode.vcxproj";
    write_file(file);

    const auto result = resolve_tui_chat_file_link(
        "build/acecode.vcxproj",
        utf8(build));

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(fs::equivalent(result.path, file));
}

TEST(TuiChatFileLink, MissingLocalTargetDoesNotCallLauncher) {
    TempDir temp("acecode_tui_link_missing");
    bool called = false;

    const auto result = open_tui_chat_file_link(
        "missing.cpp",
        utf8(temp.path()),
        [&](const fs::path&, OpenInExplorerTargetKind, std::string&) {
            called = true;
            return true;
        });

    EXPECT_TRUE(result.handled);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(called);
    EXPECT_NE(result.error.find("does not exist"), std::string::npos);
}

TEST(TuiChatFileLink, FileRevealUsesSharedLauncherWithFileKind) {
    TempDir temp("acecode_tui_link_file");
    const fs::path file = temp.path() / "result.txt";
    write_file(file);
    fs::path launched;
    auto launched_kind = OpenInExplorerTargetKind::Directory;

    const auto result = open_tui_chat_file_link(
        utf8(file),
        utf8(temp.path()),
        [&](const fs::path& path,
            OpenInExplorerTargetKind kind,
            std::string&) {
            launched = path;
            launched_kind = kind;
            return true;
        });

    ASSERT_TRUE(result.handled);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(fs::equivalent(launched, file));
    EXPECT_EQ(launched_kind, OpenInExplorerTargetKind::File);
}

TEST(TuiChatFileLink, DirectoryRevealUsesSharedLauncherWithDirectoryKind) {
    TempDir temp("acecode_tui_link_directory");
    auto launched_kind = OpenInExplorerTargetKind::File;

    const auto result = open_tui_chat_file_link(
        utf8(temp.path()),
        utf8(temp.path()),
        [&](const fs::path&,
            OpenInExplorerTargetKind kind,
            std::string&) {
            launched_kind = kind;
            return true;
        });

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(launched_kind, OpenInExplorerTargetKind::Directory);
}

TEST(TuiChatFileLink, ExternalLinkDoesNotCallLocalLauncher) {
    TempDir temp("acecode_tui_link_external_launch");
    bool called = false;

    const auto result = open_tui_chat_file_link(
        "https://example.com/file.cpp",
        utf8(temp.path()),
        [&](const fs::path&, OpenInExplorerTargetKind, std::string&) {
            called = true;
            return true;
        });

    EXPECT_FALSE(result.handled);
    EXPECT_FALSE(called);
}

} // namespace
