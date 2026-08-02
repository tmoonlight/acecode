#include "desktop/context_items.hpp"

#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

namespace fs = std::filesystem;

class ContextItemsTempDir {
public:
    ContextItemsTempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
            ("acecode-context-items-" + std::to_string(stamp));
        fs::create_directories(path);
    }

    ~ContextItemsTempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path path;
};

TEST(DesktopContextItems, MaterializesFilesAndFoldersInTransferOrder) {
    ContextItemsTempDir temp;
    const fs::path folder = temp.path / "folder with spaces";
    const fs::path file = temp.path / "notes.txt";
    fs::create_directories(folder);
    {
        std::ofstream output(file, std::ios::binary);
        output << "hello";
    }

    const auto result = acecode::desktop::materialize_context_items({
        acecode::path_to_utf8(file),
        acecode::path_to_utf8(folder),
    });

    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(result.items.size(), 2u);
    EXPECT_EQ(result.items[0].kind, acecode::desktop::ContextItemKind::File);
    EXPECT_EQ(result.items[0].name, "notes.txt");
    EXPECT_EQ(result.items[0].mime_type, "text/plain");
    EXPECT_EQ(result.items[0].bytes, "hello");
    EXPECT_EQ(result.items[0].size_bytes, 5u);
    EXPECT_TRUE(fs::path(result.items[0].path).is_absolute());
    EXPECT_EQ(result.items[1].kind, acecode::desktop::ContextItemKind::Folder);
    EXPECT_EQ(result.items[1].name, "folder with spaces");
    EXPECT_TRUE(result.items[1].bytes.empty());
}

TEST(DesktopContextItems, RejectsRelativeAndMissingPaths) {
    auto relative = acecode::desktop::materialize_context_items({"relative.txt"});
    EXPECT_FALSE(relative);
    EXPECT_NE(relative.error.find("absolute"), std::string::npos);

    ContextItemsTempDir temp;
    auto missing = acecode::desktop::materialize_context_items({
        acecode::path_to_utf8(temp.path / "missing.txt"),
    });
    EXPECT_FALSE(missing);
    EXPECT_FALSE(missing.error.empty());
}

} // namespace
