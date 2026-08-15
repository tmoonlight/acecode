#include "desktop/context_items.hpp"

#include "session/attachment_store.hpp"
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

TEST(DesktopContextItems, ReferencesOrdinaryFilesAndFoldersInTransferOrder) {
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
    EXPECT_TRUE(result.items[0].reference_only);
    EXPECT_TRUE(result.items[0].bytes.empty());
    EXPECT_EQ(result.items[0].size_bytes, 5u);
    EXPECT_TRUE(fs::path(result.items[0].path).is_absolute());
    EXPECT_EQ(result.items[1].kind, acecode::desktop::ContextItemKind::Folder);
    EXPECT_EQ(result.items[1].name, "folder with spaces");
    EXPECT_FALSE(result.items[1].reference_only);
    EXPECT_TRUE(result.items[1].bytes.empty());
}

TEST(DesktopContextItems, ReadsRasterImagesForSnapshotUpload) {
    ContextItemsTempDir temp;
    const fs::path image = temp.path / "screen.png";
    {
        std::ofstream output(image, std::ios::binary);
        output << "png-bytes";
    }

    const auto result = acecode::desktop::materialize_context_items({
        acecode::path_to_utf8(image),
    });

    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(result.items.size(), 1u);
    EXPECT_FALSE(result.items[0].reference_only);
    EXPECT_EQ(result.items[0].mime_type, "image/png");
    EXPECT_EQ(result.items[0].bytes, "png-bytes");
    EXPECT_EQ(result.items[0].size_bytes, 9u);
}

TEST(DesktopContextItems, LargeOrdinaryFileBypassesSnapshotLimit) {
    ContextItemsTempDir temp;
    const fs::path pdf = temp.path / "large.pdf";
    {
        std::ofstream output(pdf, std::ios::binary);
        output.put('x');
    }
    const auto large_size = static_cast<std::uintmax_t>(
        acecode::kMaxAttachmentBytes) + 1u;
    fs::resize_file(pdf, large_size);

    const auto result = acecode::desktop::materialize_context_items({
        acecode::path_to_utf8(pdf),
    });

    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(result.items.size(), 1u);
    EXPECT_TRUE(result.items[0].reference_only);
    EXPECT_TRUE(result.items[0].bytes.empty());
    EXPECT_EQ(result.items[0].size_bytes, large_size);
}

TEST(DesktopContextItems, LargeRasterImageRetainsSnapshotLimit) {
    ContextItemsTempDir temp;
    const fs::path image = temp.path / "large.png";
    {
        std::ofstream output(image, std::ios::binary);
        output.put('x');
    }
    fs::resize_file(image, static_cast<std::uintmax_t>(
        acecode::kMaxAttachmentBytes) + 1u);

    const auto result = acecode::desktop::materialize_context_items({
        acecode::path_to_utf8(image),
    });

    EXPECT_FALSE(result);
    EXPECT_NE(result.error.find("25 MiB"), std::string::npos);
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
