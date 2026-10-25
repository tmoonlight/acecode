// 覆盖 image::compress_image_for_model(「压缩图片」的压缩算法,参数与流程对齐
// grok-build 的 image_compress.rs / image_normalize.rs):
//   1. 尺寸、字节都在上限内的图片原样保留(Unchanged),不解码不重编码
//   2. 长边超过 max_side 时等比缩到 max_side,短边按四舍五入
//   3. 总像素超过 max_pixels 时按面积缩小,输出像素数不超过上限
//   4. 字节超限:JPEG 质量阶梯逐级下降,输出不超过 max_bytes,且绝不放大
//   5. keep_dimensions(Computer Use 截图)只看字节,尺寸绝不改变
//   6. keep_dimensions 下重编码也压不进上限 → CouldNotFit(调用方保留原图)
//   7. 带真实透明度的图片只出 PNG 候选,透明度不被 JPEG 抹掉
//   8. 不认识的格式返回 Unsupported,交给原有规范化路径处理
//   9. 进程级开关默认开启,可切换
// 测试图片用 PPM(stb 可解码)和手工拼的未压缩 PNG,全部在内存里生成。

#include <gtest/gtest.h>

#include "image/image_processor.hpp"

#include <cstdint>
#include <string>

namespace {

using acecode::image::ModelImageLimits;
using acecode::image::ModelImageOutcome;
using acecode::image::compress_image_for_model;

// 渐变图:相邻像素变化平滑,PNG / JPEG 都能压得很小。
std::string gradient_ppm(int width, int height) {
    std::string bytes = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bytes.push_back(static_cast<char>((x * 255) / std::max(1, width - 1)));
            bytes.push_back(static_cast<char>((y * 255) / std::max(1, height - 1)));
            bytes.push_back(static_cast<char>(96));
        }
    }
    return bytes;
}

// 噪声图:确定性的伪随机像素,PNG 几乎压不动,用来逼出 JPEG 质量阶梯与缩小循环。
std::string noise_ppm(int width, int height) {
    std::string bytes = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    std::uint32_t state = 0x12345678u;
    for (int i = 0; i < width * height * 3; ++i) {
        state = state * 1664525u + 1013904223u;
        bytes.push_back(static_cast<char>(state >> 24));
    }
    return bytes;
}

std::uint32_t crc32_of(const std::string& data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : data) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

void put_be32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}

void put_chunk(std::string& out, const std::string& type, const std::string& data) {
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    const std::string body = type + data;
    out += body;
    put_be32(out, crc32_of(body));
}

// 手工拼一张 RGBA PNG:zlib 用不压缩的 stored 块,alpha 从左到右 0→255 渐变。
std::string rgba_png_with_alpha(int width, int height) {
    std::string raw;
    for (int y = 0; y < height; ++y) {
        raw.push_back('\0');  // filter: none
        for (int x = 0; x < width; ++x) {
            raw.push_back(static_cast<char>(200));
            raw.push_back(static_cast<char>(40));
            raw.push_back(static_cast<char>(y * 4));
            raw.push_back(static_cast<char>((x * 255) / std::max(1, width - 1)));
        }
    }
    std::string zlib = "\x78\x01";
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const std::size_t len = std::min<std::size_t>(65535, raw.size() - offset);
        const bool last = offset + len == raw.size();
        zlib.push_back(static_cast<char>(last ? 1 : 0));
        zlib.push_back(static_cast<char>(len & 0xFF));
        zlib.push_back(static_cast<char>(len >> 8));
        zlib.push_back(static_cast<char>(~len & 0xFF));
        zlib.push_back(static_cast<char>((~len >> 8) & 0xFF));
        zlib.append(raw, offset, len);
        offset += len;
    }
    std::uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    put_be32(zlib, (b << 16) | a);

    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    put_be32(ihdr, static_cast<std::uint32_t>(width));
    put_be32(ihdr, static_cast<std::uint32_t>(height));
    ihdr += std::string("\x08\x06\x00\x00\x00", 5);  // 8 bit, RGBA
    put_chunk(png, "IHDR", ihdr);
    put_chunk(png, "IDAT", zlib);
    put_chunk(png, "IEND", "");
    return png;
}

ModelImageLimits roomy_limits() {
    ModelImageLimits limits;
    limits.max_bytes = 64u * 1024u * 1024u;
    limits.max_side = 4000;
    limits.max_pixels = 100'000'000;
    limits.min_side = 16;
    return limits;
}

} // namespace

// 场景 1:64x32 的小渐变图,默认上限(1.5 MB / 2000 px / 2,408,448 像素)。
// 期望:Unchanged,input / output 都是原尺寸,不产生任何新字节。
TEST(ModelImageCompression, ImageWithinLimitsIsUnchanged) {
    const auto result = compress_image_for_model(gradient_ppm(64, 32), "image/png");
    EXPECT_EQ(result.outcome, ModelImageOutcome::Unchanged);
    EXPECT_EQ(result.input.width, 64);
    EXPECT_EQ(result.input.height, 32);
    EXPECT_TRUE(result.bytes.empty());
}

// 场景 2:300x100 渐变图,max_side=120,字节与像素上限都很宽。
// 期望:等比缩到 120x40(短边 100*0.4=40),输出字节不超过上限。
TEST(ModelImageCompression, LongSideIsCappedAndAspectRatioKept) {
    auto limits = roomy_limits();
    limits.max_side = 120;
    const auto result = compress_image_for_model(gradient_ppm(300, 100), "image/png", false, limits);
    ASSERT_EQ(result.outcome, ModelImageOutcome::Compressed) << result.error;
    EXPECT_EQ(result.output.width, 120);
    EXPECT_EQ(result.output.height, 40);
    EXPECT_LE(result.bytes.size(), limits.max_bytes);
}

// 场景 3:200x200(4 万像素),max_pixels=10000,边长上限很宽。
// 期望:按面积缩到 100x100;输出像素数不超过 max_pixels(grok-build 的
// area_capped_side 会在四舍五入超出时逐像素回退,这里正好整除)。
TEST(ModelImageCompression, PixelAreaIsCapped) {
    auto limits = roomy_limits();
    limits.max_pixels = 10'000;
    const auto result = compress_image_for_model(gradient_ppm(200, 200), "image/png", false, limits);
    ASSERT_EQ(result.outcome, ModelImageOutcome::Compressed) << result.error;
    EXPECT_LE(static_cast<std::uint64_t>(result.output.width) * result.output.height,
              limits.max_pixels);
    EXPECT_EQ(result.output.width, 100);
    EXPECT_EQ(result.output.height, 100);
}

// 场景 4:160x120 噪声图(PPM 原始 57 KB),max_bytes=20000,尺寸上限很宽。
// 期望:Compressed、不超过 20000 字节;噪声 PNG 压不进上限,所以是 JPEG;
// 尺寸只可能变小不会变大(grok-build:never upscale)。
TEST(ModelImageCompression, ByteLimitUsesJpegQualityLadderWithoutUpscaling) {
    auto limits = roomy_limits();
    limits.max_bytes = 20'000;
    const auto result = compress_image_for_model(noise_ppm(160, 120), "image/png", false, limits);
    ASSERT_EQ(result.outcome, ModelImageOutcome::Compressed) << result.error;
    EXPECT_LE(result.bytes.size(), limits.max_bytes);
    EXPECT_EQ(result.mime_type, "image/jpeg");
    EXPECT_LE(result.output.width, 160);
    EXPECT_LE(result.output.height, 120);
}

// 场景 5:同一张噪声图按 Computer Use 截图处理(keep_dimensions),max_side=50
// 比图片小得多、max_bytes=40000 足够放下一张中等质量 JPEG。
// 期望:尺寸保持 160x120 —— 截图坐标依赖像素尺寸,max_side / max_pixels 不生效。
TEST(ModelImageCompression, KeepDimensionsNeverResizes) {
    auto limits = roomy_limits();
    limits.max_side = 50;
    limits.max_pixels = 100;
    limits.max_bytes = 40'000;
    const auto result = compress_image_for_model(noise_ppm(160, 120), "image/png", true, limits);
    ASSERT_EQ(result.outcome, ModelImageOutcome::Compressed) << result.error;
    EXPECT_EQ(result.output.width, 160);
    EXPECT_EQ(result.output.height, 120);
    EXPECT_LE(result.bytes.size(), limits.max_bytes);
}

// 场景 6:keep_dimensions 且 max_bytes=100,最低质量的 JPEG 也放不下。
// 期望:CouldNotFit(不缩小重试),调用方据此保留原图;error 说明压不进上限。
TEST(ModelImageCompression, KeepDimensionsReportsCouldNotFit) {
    auto limits = roomy_limits();
    limits.max_bytes = 100;
    const auto result = compress_image_for_model(noise_ppm(160, 120), "image/png", true, limits);
    EXPECT_EQ(result.outcome, ModelImageOutcome::CouldNotFit);
    EXPECT_TRUE(result.bytes.empty());
    EXPECT_NE(result.error.find("could not fit"), std::string::npos) << result.error;
}

// 场景 7:64x64 RGBA PNG,alpha 横向渐变(未压缩约 16 KB),max_bytes=8000。
// 期望:输出 image/png 且 has_alpha=true —— JPEG 会把透明区域压成实色,
// 所以带真实透明度的图片只尝试 PNG。
TEST(ModelImageCompression, RealAlphaStaysPng) {
    auto limits = roomy_limits();
    limits.max_bytes = 8'000;
    const std::string png = rgba_png_with_alpha(64, 64);
    ASSERT_GT(png.size(), limits.max_bytes);
    const auto result = compress_image_for_model(png, "image/png", false, limits);
    ASSERT_EQ(result.outcome, ModelImageOutcome::Compressed) << result.error;
    EXPECT_EQ(result.mime_type, "image/png");
    EXPECT_TRUE(result.output.has_alpha);
}

// 场景 8:WebP / 非图片字节。
// 期望:Unsupported —— stb 解不了 WebP;调用方会退回原有的 normalize_image_bytes 路径,
// 行为与关闭「压缩图片」时一致。
TEST(ModelImageCompression, UndecodableInputIsUnsupported) {
    EXPECT_EQ(compress_image_for_model("RIFF....WEBP", "image/webp").outcome,
              ModelImageOutcome::Unsupported);
    EXPECT_EQ(compress_image_for_model("not an image", "image/png").outcome,
              ModelImageOutcome::Unsupported);
}

// 场景 9:进程级「压缩图片」开关。期望:默认开启(与 config 默认一致),可关可开。
TEST(ModelImageCompression, ProcessSwitchDefaultsOnAndToggles) {
    EXPECT_TRUE(acecode::image::model_image_compression_enabled());
    acecode::image::set_model_image_compression_enabled(false);
    EXPECT_FALSE(acecode::image::model_image_compression_enabled());
    acecode::image::set_model_image_compression_enabled(true);
    EXPECT_TRUE(acecode::image::model_image_compression_enabled());
}
