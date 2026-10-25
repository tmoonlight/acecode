// 覆盖 session/attachment_model_image(「压缩图片」在附件层的压缩缓存):
//   1. 长边超过 2000 的图片附件:按 grok-build 上限压到长边 2000,快照原图不动,
//      旁边写出 <快照>.model-v1.json / .bin 两个附属文件
//   2. 清空内存缓存后再取:直接读附属文件,结果与第一次逐字节相同(重启不重算)
//   3. 附属文件记录的源文件大小对不上(快照被替换过)时忽略它、重新压缩
//   4. 本来就在上限内的小图:不压缩,只写一个 compressed=false 的 json,不写 .bin
//   5. 带 computer_use 元数据的截图:尺寸保持不变(截图坐标依赖像素尺寸)
//   6. 发送体积估算 = data URL 长度(压缩后字节的 base64 + 前缀)
//   7. 发给模型的说明文字带原尺寸 / 原大小与压缩后的尺寸 / 大小
// 附件写在系统临时目录下的独立项目目录里,不碰用户数据。

#include <gtest/gtest.h>

#include "session/attachment_model_image.hpp"
#include "session/attachment_store.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string gradient_ppm(int width, int height) {
    std::string bytes = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bytes.push_back(static_cast<char>((x * 255) / std::max(1, width - 1)));
            bytes.push_back(static_cast<char>((y * 255) / std::max(1, height - 1)));
            bytes.push_back(static_cast<char>(64));
        }
    }
    return bytes;
}

class AttachmentModelImageTest : public testing::Test {
protected:
    fs::path project = fs::temp_directory_path() /
        ("acecode_model_image_" + std::to_string(std::random_device{}()));

    void SetUp() override {
        fs::create_directories(project);
        acecode::clear_attachment_model_image_cache_for_tests();
    }
    void TearDown() override {
        acecode::clear_attachment_model_image_cache_for_tests();
        std::error_code ec;
        fs::remove_all(project, ec);
    }

    acecode::AttachmentRecord save(const std::string& bytes,
                                   const nlohmann::json& metadata = nlohmann::json::object()) {
        std::string error;
        auto record = acecode::save_attachment(acecode::path_to_utf8(project), "sid-model-image",
                                               "shot.png", "image/png", bytes, &error, metadata);
        EXPECT_TRUE(record.has_value()) << error;
        return record.value_or(acecode::AttachmentRecord{});
    }

    static fs::path sidecar(const acecode::AttachmentRecord& record, const char* extension) {
        fs::path path = acecode::path_from_utf8(record.path);
        path += ".model-v1";
        path += extension;
        return path;
    }
};

} // namespace

// 场景 1:2400x300 的渐变图(存入时不触发原有的 10 MB / 2560 px 规范化,快照保持原样)。
// 期望:compressed=true,输出长边 2000、短边 250;快照文件大小不变;两个附属文件都在。
TEST_F(AttachmentModelImageTest, OversizedImageIsCompressedAndSnapshotKept) {
    const std::string original = gradient_ppm(2400, 300);
    const auto record = save(original);
    const auto snapshot_size = fs::file_size(acecode::path_from_utf8(record.path));

    std::string error;
    const auto image = acecode::attachment_image_for_model(record, &error);
    ASSERT_TRUE(image) << error;
    EXPECT_TRUE(image->compressed);
    EXPECT_EQ(image->original.width, 2400);
    EXPECT_EQ(image->output.width, 2000);
    EXPECT_EQ(image->output.height, 250);
    EXPECT_LE(image->bytes.size(), 1'500'000u);
    EXPECT_EQ(fs::file_size(acecode::path_from_utf8(record.path)), snapshot_size);
    EXPECT_TRUE(fs::exists(sidecar(record, ".json")));
    EXPECT_TRUE(fs::exists(sidecar(record, ".bin")));
}

// 场景 2:场景 1 之后清空进程内缓存(模拟重启)再取一次。
// 期望:从附属文件读回,字节与 MIME 与第一次完全相同 —— 修复前每次重启都要把整段
// 历史里的图重新解码压缩一遍,首个请求会多等好几秒。
TEST_F(AttachmentModelImageTest, SidecarIsReusedAfterRestart) {
    const auto record = save(gradient_ppm(2400, 300));
    const auto first = acecode::attachment_image_for_model(record);
    ASSERT_TRUE(first && first->compressed);

    acecode::clear_attachment_model_image_cache_for_tests();
    const auto second = acecode::attachment_image_for_model(record);
    ASSERT_TRUE(second && second->compressed);
    EXPECT_EQ(second->bytes, first->bytes);
    EXPECT_EQ(second->mime_type, first->mime_type);
    EXPECT_EQ(second->output.width, first->output.width);
}

// 场景 3:把附属 json 里的 source_size 改成别的值(相当于快照被换过)。
// 期望:附属文件被忽略,重新压缩后仍得到长边 2000 的结果,并把 json 改写回正确的大小。
TEST_F(AttachmentModelImageTest, StaleSidecarIsIgnored) {
    const auto record = save(gradient_ppm(2400, 300));
    ASSERT_TRUE(acecode::attachment_image_for_model(record));
    {
        std::ofstream out(sidecar(record, ".json"), std::ios::binary | std::ios::trunc);
        out << R"({"version":1,"source_size":1,"compressed":true,"mime_type":"image/png","bytes":3})";
    }
    acecode::clear_attachment_model_image_cache_for_tests();
    const auto image = acecode::attachment_image_for_model(record);
    ASSERT_TRUE(image && image->compressed);
    EXPECT_EQ(image->output.width, 2000);

    std::ifstream in(sidecar(record, ".json"), std::ios::binary);
    const auto json = nlohmann::json::parse(in);
    EXPECT_EQ(json.value("source_size", std::uintmax_t{0}),
              fs::file_size(acecode::path_from_utf8(record.path)));
}

// 场景 4:64x32 的小图,本来就在所有上限内。
// 期望:compressed=false,不写 .bin;json 记下 compressed=false,下次启动不必再解码。
TEST_F(AttachmentModelImageTest, ImageWithinLimitsIsSentAsIs) {
    const auto record = save(gradient_ppm(64, 32));
    const auto image = acecode::attachment_image_for_model(record);
    ASSERT_TRUE(image);
    EXPECT_FALSE(image->compressed);
    EXPECT_TRUE(image->bytes.empty());
    EXPECT_FALSE(fs::exists(sidecar(record, ".bin")));
    std::ifstream in(sidecar(record, ".json"), std::ios::binary);
    EXPECT_FALSE(nlohmann::json::parse(in).value("compressed", true));
}

// 场景 5:同样 2400x300 的图,但带 computer_use 元数据(Computer Use 截图)。
// 期望:keeps_dimensions=true;即使长边超过 2000 也不缩小,输出仍是 2400x300
// (渐变图原始 PPM 约 2.1 MB 超过 1.5 MB,所以会按原尺寸重编码)。
TEST_F(AttachmentModelImageTest, ComputerUseScreenshotKeepsDimensions) {
    const nlohmann::json metadata = {{"computer_use", {{"observation_id", "o1"},
                                                       {"screenshot_id", "s1"}}}};
    const auto record = save(gradient_ppm(2400, 300), metadata);
    EXPECT_TRUE(acecode::attachment_image_keeps_dimensions(record));
    const auto image = acecode::attachment_image_for_model(record);
    ASSERT_TRUE(image);
    ASSERT_TRUE(image->compressed);
    EXPECT_EQ(image->output.width, 2400);
    EXPECT_EQ(image->output.height, 300);
}

// 场景 6:压缩后的发送体积。期望:4 * ceil(n / 3) + 32(data URL 前缀的估算)。
TEST_F(AttachmentModelImageTest, WireBytesUseCompressedSize) {
    const auto record = save(gradient_ppm(2400, 300));
    const auto image = acecode::attachment_image_for_model(record);
    ASSERT_TRUE(image && image->compressed);
    const std::size_t n = image->bytes.size();
    EXPECT_EQ(acecode::attachment_image_wire_bytes(record), 4 * ((n + 2) / 3) + 32);
}

// 场景 7:说明文字。期望:同时带原尺寸、压缩后尺寸,并提示细节可能丢失。
TEST_F(AttachmentModelImageTest, NoteNamesOriginalAndCompressedSize) {
    const auto record = save(gradient_ppm(2400, 300));
    const auto image = acecode::attachment_image_for_model(record);
    ASSERT_TRUE(image && image->compressed);
    const std::string note = acecode::attachment_model_image_note(*image);
    EXPECT_NE(note.find("2400x300"), std::string::npos) << note;
    EXPECT_NE(note.find("-> 2000x250"), std::string::npos) << note;
    EXPECT_NE(note.find("Fine details may have been lost"), std::string::npos) << note;
}
