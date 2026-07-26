#include <gtest/gtest.h>

#include "session/output_attachments.hpp"
#include "tool/show_image_tool.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

struct TempTree {
    fs::path path;

    TempTree() {
        path = fs::temp_directory_path() /
               ("acecode_show_image_" + std::to_string(std::random_device{}()));
        fs::create_directories(path);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& path, const std::string& bytes = "image-bytes") {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST(ShowImageTool, DefinitionIsReadOnlyAndRequiresImagePath) {
    auto tool = acecode::create_show_image_tool();

    EXPECT_EQ(tool.definition.name, "show_image");
    EXPECT_TRUE(tool.is_read_only);
    ASSERT_TRUE(tool.definition.parameters.contains("required"));
    EXPECT_EQ(
        tool.definition.parameters["required"],
        nlohmann::json::array({"image_path"}));
}

TEST(ShowImageTool, RelativeUtf8ImageProducesMaterializableAttachment) {
    TempTree temp;
    const fs::path image =
        temp.path / acecode::path_from_utf8(u8"项目/预览.PNG");
    write_file(image);

    acecode::ToolContext context;
    context.cwd = acecode::path_to_utf8(temp.path);
    const auto result = acecode::create_show_image_tool().execute(
        nlohmann::json{
            {"image_path", u8"项目/预览.PNG"},
            {"title", u8"最终预览"},
        }.dump(),
        context);

    ASSERT_TRUE(result.success) << result.output;
    ASSERT_TRUE(result.summary.has_value());
    EXPECT_EQ(result.summary->verb, "Showed");
    EXPECT_EQ(result.summary->object, u8"最终预览");
    ASSERT_TRUE(result.has_attachments());
    ASSERT_EQ(result.attachments.size(), 1u);
    EXPECT_EQ(result.attachments[0]["name"], u8"预览.PNG");
    EXPECT_EQ(result.attachments[0]["mime_type"], "image/png");
    EXPECT_EQ(
        result.attachments[0]["path"],
        acecode::path_to_utf8(image.lexically_normal()));
    EXPECT_TRUE(acecode::is_valid_utf8(result.output));

    const fs::path attachment_store = temp.path / "attachment-store";
    const auto materialized = acecode::materialize_output_attachments(
        result.attachments,
        acecode::path_to_utf8(attachment_store),
        "session-show-image",
        {},
        context.cwd);

    ASSERT_TRUE(materialized.warnings.empty())
        << nlohmann::json(materialized.warnings).dump();
    ASSERT_EQ(materialized.attachments.size(), 1u);
    EXPECT_EQ(materialized.attachments[0]["name"], u8"预览.PNG");
    EXPECT_EQ(materialized.attachments[0]["mime_type"], "image/png");
    EXPECT_FALSE(materialized.attachments[0]["blob_url"].get<std::string>().empty());
    EXPECT_TRUE(fs::is_regular_file(
        acecode::path_from_utf8(
            materialized.attachments[0]["path"].get<std::string>())));
}

TEST(ShowImageTool, FilenameBecomesDefaultSummaryObject) {
    TempTree temp;
    const fs::path image = temp.path / "diagram.webp";
    write_file(image, "webp");

    const auto result = acecode::create_show_image_tool().execute(
        nlohmann::json{{"image_path", acecode::path_to_utf8(image)}}.dump(),
        acecode::ToolContext{});

    ASSERT_TRUE(result.success) << result.output;
    ASSERT_TRUE(result.summary.has_value());
    EXPECT_EQ(result.summary->object, "diagram.webp");
    EXPECT_EQ(result.attachments[0]["mime_type"], "image/webp");
}

TEST(ShowImageTool, MissingPathFailsWithoutAttachment) {
    const auto result = acecode::create_show_image_tool().execute(
        nlohmann::json::object().dump(),
        acecode::ToolContext{});

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("image_path"), std::string::npos);
    EXPECT_FALSE(result.has_attachments());
}

TEST(ShowImageTool, MissingFileFailsWithoutAttachment) {
    TempTree temp;
    acecode::ToolContext context;
    context.cwd = acecode::path_to_utf8(temp.path);

    const auto result = acecode::create_show_image_tool().execute(
        R"({"image_path":"missing.png"})",
        context);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("File not found"), std::string::npos);
    EXPECT_FALSE(result.has_attachments());
}

TEST(ShowImageTool, DirectoryAndUnsupportedFormatAreRejected) {
    TempTree temp;
    const fs::path image_directory = temp.path / "folder.png";
    fs::create_directories(image_directory);
    const fs::path svg = temp.path / "diagram.svg";
    write_file(svg, "<svg/>");
    acecode::ToolContext context;
    context.cwd = acecode::path_to_utf8(temp.path);
    auto tool = acecode::create_show_image_tool();

    const auto directory_result = tool.execute(
        R"({"image_path":"folder.png"})",
        context);
    EXPECT_FALSE(directory_result.success);
    EXPECT_NE(directory_result.output.find("not a regular file"), std::string::npos);
    EXPECT_FALSE(directory_result.has_attachments());

    const auto svg_result = tool.execute(
        R"({"image_path":"diagram.svg"})",
        context);
    EXPECT_FALSE(svg_result.success);
    EXPECT_NE(svg_result.output.find("supported formats"), std::string::npos);
    EXPECT_FALSE(svg_result.has_attachments());
}
