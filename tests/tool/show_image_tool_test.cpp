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

namespace {

// 4x2 的 PPM 内容;stb 按内容识别格式,所以可以存成 .png 交给 show_image。
std::string ppm_image_4x2() {
    std::string bytes = "P6\n4 2\n255\n";
    for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<char>(10 + i * 30));
        bytes.push_back(static_cast<char>(200 - i * 20));
        bytes.push_back(static_cast<char>(i * 7));
    }
    return bytes;
}

} // namespace

// 场景:模型看过一张截图后,只想把其中一块区域给用户看(IM 通道里会作为图片发回聊天)。
// 期望:带 crop 时附件是截出来的 PNG(data URL,不依赖临时文件),名字是「原名-crop.png」,
// 摘要带截取尺寸,能正常存进会话附件库;原图文件不变。
TEST(ShowImageTool, CropShowsOnlyTheRequestedRegion) {
    TempTree temp;
    const fs::path image = temp.path / "screen.png";
    write_file(image, ppm_image_4x2());

    acecode::ToolContext context;
    context.cwd = acecode::path_to_utf8(temp.path);
    const auto result = acecode::create_show_image_tool().execute(
        nlohmann::json{
            {"image_path", "screen.png"},
            {"crop", {{"x", 1}, {"y", 0}, {"width", 2}, {"height", 2}}},
        }.dump(),
        context);

    ASSERT_TRUE(result.success) << result.output;
    ASSERT_EQ(result.attachments.size(), 1u);
    EXPECT_EQ(result.attachments[0]["name"], "screen-crop.png");
    EXPECT_EQ(result.attachments[0]["mime_type"], "image/png");
    EXPECT_EQ(result.attachments[0].value("data_url", std::string{}).rfind("data:image/png;base64,", 0), 0u);
    EXPECT_FALSE(result.attachments[0].contains("path"));
    EXPECT_NE(result.output.find("cropped to 2x2 at 1,0 of 4x2"), std::string::npos);
    ASSERT_TRUE(result.summary.has_value());
    ASSERT_FALSE(result.summary->metrics.empty());
    EXPECT_EQ(result.summary->metrics.back().second, "2x2");

    const auto materialized = acecode::materialize_output_attachments(
        result.attachments,
        acecode::path_to_utf8(temp.path / "attachment-store"),
        "session-show-image-crop",
        {},
        context.cwd);
    ASSERT_TRUE(materialized.warnings.empty())
        << nlohmann::json(materialized.warnings).dump();
    ASSERT_EQ(materialized.attachments.size(), 1u);
    EXPECT_EQ(materialized.attachments[0]["mime_type"], "image/png");
    EXPECT_EQ(fs::file_size(image), ppm_image_4x2().size());
}

// 场景:crop 缺字段、区域整个落在图外。
// 期望:都报 crop 参数错误且不产生附件;图外的错误里带原图尺寸,模型可以据此重试。
TEST(ShowImageTool, InvalidCropFailsWithoutAttachment) {
    TempTree temp;
    const fs::path image = temp.path / "screen.png";
    write_file(image, ppm_image_4x2());
    acecode::ToolContext context;
    context.cwd = acecode::path_to_utf8(temp.path);

    const auto missing = acecode::create_show_image_tool().execute(
        nlohmann::json{{"image_path", "screen.png"}, {"crop", {{"x", 0}, {"y", 0}}}}.dump(),
        context);
    EXPECT_FALSE(missing.success);
    EXPECT_NE(missing.output.find("crop"), std::string::npos);
    EXPECT_FALSE(missing.has_attachments());

    const auto outside = acecode::create_show_image_tool().execute(
        nlohmann::json{
            {"image_path", "screen.png"},
            {"crop", {{"x", 10}, {"y", 0}, {"width", 2}, {"height", 2}}},
        }.dump(),
        context);
    EXPECT_FALSE(outside.success);
    EXPECT_NE(outside.output.find("4x2"), std::string::npos);
    EXPECT_FALSE(outside.has_attachments());
}
