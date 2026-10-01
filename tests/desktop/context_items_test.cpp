#include "desktop/context_items.hpp"

#include "session/attachment_store.hpp"
#include "utils/utf8_path.hpp"
#include "utils/base64.hpp"

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

// 触发场景:Desktop 拖入 / 选择 / 从资源管理器复制一张 25 MiB 以内的本地图片。
// 期望行为:条目带上图片字节(reference_only=false),前端据此走快照附件:输入框与
// 对话记录显示缩略图,模型直接收到图片;来源路径仍随条目返回。
// 回归:曾经图片也只给路径,前端只能插 @路径,输入框和对话记录都只剩文件名
// (服务端拒收图片引用,模型要先 bash 再 show_image 才看得到)。
TEST(DesktopContextItems, RasterImagesCarrySnapshotBytes) {
    ContextItemsTempDir temp;
    const fs::path image = temp.path / "screen.png";
    const std::string bytes("png\0bytes", 9);
    {
        std::ofstream output(image, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    const auto result = acecode::desktop::materialize_context_items({
        acecode::path_to_utf8(image),
    });

    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(result.items.size(), 1u);
    EXPECT_FALSE(result.items[0].reference_only);
    EXPECT_EQ(result.items[0].mime_type, "image/png");
    EXPECT_EQ(result.items[0].bytes, bytes);
    EXPECT_EQ(result.items[0].size_bytes, bytes.size());
    EXPECT_TRUE(fs::path(result.items[0].path).is_absolute());
}

// 触发场景:拖入 SVG。
// 期望行为:SVG 不是栅格图,不读字节,仍是路径引用(与前端 isRasterImageMimeType 一致)。
TEST(DesktopContextItems, SvgStaysPathReference) {
    ContextItemsTempDir temp;
    const fs::path svg = temp.path / "logo.svg";
    std::ofstream(svg, std::ios::binary) << "<svg/>";

    const auto result = acecode::desktop::materialize_context_items({
        acecode::path_to_utf8(svg),
    });

    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(result.items.size(), 1u);
    EXPECT_EQ(result.items[0].mime_type, "image/svg+xml");
    EXPECT_TRUE(result.items[0].reference_only);
    EXPECT_TRUE(result.items[0].bytes.empty());
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

// 触发场景:拖入超过 25 MiB 快照上限的图片。
// 期望行为:不读字节、不报错,退回路径引用(添加文件本身不因大小失败)。
TEST(DesktopContextItems, LargeRasterImageBypassesSnapshotLimit) {
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

    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(result.items.size(), 1u);
    EXPECT_TRUE(result.items[0].reference_only);
    EXPECT_TRUE(result.items[0].bytes.empty());
    EXPECT_EQ(result.items[0].size_bytes, static_cast<std::uintmax_t>(
        acecode::kMaxAttachmentBytes) + 1u);
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

TEST(DesktopContextItems, SavesPathlessDataAsPersistentPathReferences) {
    ContextItemsTempDir temp;
    const std::string bytes("png\0bytes", 9);
    const auto result = acecode::desktop::store_context_data_files(
        acecode::path_to_utf8(temp.path), {
            {"截图.png", acecode::base64_encode(bytes)},
            {"截图.png", acecode::base64_encode("second")},
            {"..\\outside.txt", acecode::base64_encode("safe")},
        });
    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(result.items.size(), 3u);
    EXPECT_EQ(result.items[0].name, "截图.png");
    // 落盘后的图片同样按栅格图规则带回字节(前端现在直接上传截图,这里只是兜底)。
    EXPECT_FALSE(result.items[0].reference_only);
    EXPECT_EQ(result.items[0].bytes, bytes);
    EXPECT_EQ(result.items[0].size_bytes, bytes.size());
    EXPECT_NE(result.items[0].path, result.items[1].path);
    EXPECT_EQ(result.items[2].name, "outside.txt");
    EXPECT_TRUE(result.items[2].reference_only);
    for (const auto& item : result.items) {
        const auto path = acecode::path_from_utf8(item.path);
        EXPECT_EQ(path.parent_path().parent_path(), fs::weakly_canonical(temp.path));
        EXPECT_TRUE(fs::is_regular_file(path));
    }
    std::ifstream input(acecode::path_from_utf8(result.items[0].path), std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), bytes);
}

TEST(DesktopContextItems, FailedDataBatchRemovesOnlyItsOwnFiles) {
    ContextItemsTempDir temp;
    const auto keep = temp.path / "existing.txt";
    std::ofstream(keep) << "keep";
    const auto result = acecode::desktop::store_context_data_files(
        acecode::path_to_utf8(temp.path), {
            {"valid.txt", acecode::base64_encode("data")}, {"invalid.txt", "!!!!"},
        });
    EXPECT_FALSE(result);
    EXPECT_TRUE(result.items.empty());
    EXPECT_TRUE(fs::exists(keep));
    EXPECT_EQ(std::distance(fs::directory_iterator(temp.path), fs::directory_iterator{}), 1);
}

} // namespace
