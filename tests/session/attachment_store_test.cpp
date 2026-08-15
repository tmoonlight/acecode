#include <gtest/gtest.h>

#include "session/attachment_store.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path unique_tmp_dir(const std::string& suffix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = fs::temp_directory_path() /
        ("acecode_attachment_store_" + suffix + "_" + std::to_string(now));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST(AttachmentStore, SaveLoadAndReadImageAttachment) {
    auto dir = unique_tmp_dir("image");
    const std::string project_dir = acecode::path_to_utf8(dir);

    std::string error;
    auto saved = acecode::save_attachment(
        project_dir,
        "session1",
        "screen.png",
        "image/png",
        std::string{"png-bytes"},
        &error);

    ASSERT_TRUE(saved.has_value()) << error;
    EXPECT_EQ(saved->kind, "image");
    EXPECT_EQ(saved->mime_type, "image/png");
    EXPECT_NE(saved->blob_url.find("/api/sessions/session1/attachments/"), std::string::npos);

    auto loaded = acecode::load_attachment(project_dir, "session1", saved->id, &error);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_EQ(loaded->id, saved->id);
    EXPECT_EQ(loaded->path, saved->path);

    auto bytes = acecode::read_attachment_bytes(*loaded, acecode::kMaxAttachmentBytes, &error);
    ASSERT_TRUE(bytes.has_value()) << error;
    EXPECT_EQ(*bytes, "png-bytes");

    fs::remove_all(dir);
}

TEST(AttachmentStore, RejectsInvalidAttachmentId) {
    auto dir = unique_tmp_dir("invalid");
    std::string error;
    auto loaded = acecode::load_attachment(
        acecode::path_to_utf8(dir),
        "session1",
        "../nope",
        &error);

    EXPECT_FALSE(loaded.has_value());
    EXPECT_EQ(error, "invalid attachment id");

    fs::remove_all(dir);
}

TEST(AttachmentStore, PersistsInitialSourcePathMetadata) {
    auto dir = unique_tmp_dir("source_path");
    const std::string project_dir = acecode::path_to_utf8(dir);
    const std::string source_path = acecode::path_to_utf8(dir / "source.txt");

    std::string error;
    auto saved = acecode::save_attachment(
        project_dir,
        "session1",
        "source.txt",
        "text/plain",
        std::string{"source bytes"},
        &error,
        nlohmann::json{{"source_path", source_path}});

    ASSERT_TRUE(saved.has_value()) << error;
    EXPECT_EQ(saved->metadata.value("source_path", ""), source_path);
    auto loaded = acecode::load_attachment(project_dir, "session1", saved->id, &error);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_EQ(loaded->metadata.value("source_path", ""), source_path);

    fs::remove_all(dir);
}

TEST(AttachmentStore, PersistsLargeOrdinaryFileAsMetadataOnlyReference) {
    auto dir = unique_tmp_dir("source_reference");
    const std::string project_dir = acecode::path_to_utf8(dir / "project");
    const fs::path source = dir / "large.pdf";
    {
        std::ofstream output(source, std::ios::binary);
        output.put('x');
    }
    const auto source_size = static_cast<std::uintmax_t>(
        acecode::kMaxAttachmentBytes) + 4096u;
    fs::resize_file(source, source_size);

    std::string error;
    auto saved = acecode::save_attachment_reference(
        project_dir,
        "session1",
        "large.pdf",
        "application/pdf",
        acecode::path_to_utf8(source),
        &error);

    ASSERT_TRUE(saved.has_value()) << error;
    EXPECT_EQ(saved->kind, "file");
    EXPECT_EQ(saved->size_bytes, source_size);
    EXPECT_TRUE(saved->path.empty());
    EXPECT_TRUE(saved->blob_url.empty());
    EXPECT_EQ(saved->metadata.value("storage", ""), "source_reference");
    EXPECT_EQ(saved->metadata.value("source_path", ""),
              acecode::path_to_utf8_generic(fs::weakly_canonical(source)));

    auto loaded = acecode::load_attachment(
        project_dir, "session1", saved->id, &error);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_TRUE(loaded->path.empty());
    EXPECT_TRUE(loaded->blob_url.empty());
    EXPECT_EQ(loaded->size_bytes, source_size);

    auto bytes = acecode::read_attachment_bytes(
        *loaded, acecode::kMaxAttachmentBytes, &error);
    EXPECT_FALSE(bytes.has_value());
    EXPECT_EQ(error, "attachment path missing");

    const fs::path stored_dir = acecode::path_from_utf8(project_dir) /
        "attachments" / "session1";
    std::size_t stored_file_count = 0;
    for (const auto& entry : fs::directory_iterator(stored_dir)) {
        if (entry.is_regular_file()) ++stored_file_count;
    }
    EXPECT_EQ(stored_file_count, 1u);

    fs::remove_all(dir);
}

TEST(AttachmentStore, RejectsImagesAndInvalidPathsForSourceReferences) {
    auto dir = unique_tmp_dir("source_reference_invalid");
    const fs::path image = dir / "screen.png";
    {
        std::ofstream output(image, std::ios::binary);
        output << "png";
    }

    std::string error;
    auto image_reference = acecode::save_attachment_reference(
        acecode::path_to_utf8(dir / "project"),
        "session1",
        "renamed.pdf",
        "application/pdf",
        acecode::path_to_utf8(image),
        &error);
    EXPECT_FALSE(image_reference.has_value());
    EXPECT_EQ(error, "image attachments require snapshot data");

    auto relative_reference = acecode::save_attachment_reference(
        acecode::path_to_utf8(dir / "project"),
        "session1",
        "notes.txt",
        "text/plain",
        "relative/notes.txt",
        &error);
    EXPECT_FALSE(relative_reference.has_value());
    EXPECT_EQ(error, "attachment source path must be absolute");

    fs::remove_all(dir);
}

TEST(AttachmentStore, RejectsLargeImageWhenNormalizationFails) {
    auto dir = unique_tmp_dir("large_image");
    const std::string project_dir = acecode::path_to_utf8(dir);

    std::string error;
    auto saved = acecode::save_attachment(
        project_dir,
        "session1",
        "broken.png",
        "image/png",
        std::string(10u * 1024u * 1024u, 'x'),
        &error);

    EXPECT_FALSE(saved.has_value());
    EXPECT_NE(error.find("image normalization failed"), std::string::npos);

    fs::remove_all(dir);
}
