#include <gtest/gtest.h>

#include "image/image_processor.hpp"

#include <string>

namespace {

std::string ppm_image_4x2() {
    std::string bytes = "P6\n4 2\n255\n";
    for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<char>(i % 2 ? 255 : 16));
        bytes.push_back(static_cast<char>(i % 3 ? 64 : 220));
        bytes.push_back(static_cast<char>(i % 4 ? 180 : 32));
    }
    return bytes;
}

} // namespace

TEST(ImageProcessor, BelowThresholdDoesNotNormalize) {
    const std::string bytes = ppm_image_4x2();
    acecode::image::ImageNormalizeOptions opts;
    opts.compression_threshold_bytes = 1024 * 1024;
    opts.max_edge = 100;

    auto result = acecode::image::normalize_image_bytes(bytes, "image/png", opts);
    EXPECT_FALSE(result.attempted);
    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.reason, "below thresholds");
}

TEST(ImageProcessor, DimensionThresholdUsesStbResize) {
    const std::string bytes = ppm_image_4x2();
    acecode::image::ImageNormalizeOptions opts;
    opts.compression_threshold_bytes = 1024 * 1024;
    opts.max_edge = 2;

    auto result = acecode::image::normalize_image_bytes(bytes, "image/png", opts);
    ASSERT_TRUE(result.attempted);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.changed) << result.reason;
    EXPECT_EQ(result.backend, "stb");
    EXPECT_EQ(result.output.width, 2);
    EXPECT_EQ(result.output.height, 1);
    EXPECT_EQ(result.mime_type, "image/jpeg");

    std::string error;
    auto info = acecode::image::probe_image_info(result.bytes, &error);
    ASSERT_TRUE(info.has_value()) << error;
    EXPECT_EQ(info->width, 2);
    EXPECT_EQ(info->height, 1);
}

TEST(ImageProcessor, TenMiBIsCompressionThreshold) {
    EXPECT_EQ(acecode::image::kImageCompressionThresholdBytes,
              10u * 1024u * 1024u);
}

TEST(ImageProcessor, ForcedPngConvertsOpaqueImagesWithoutCompressionThreshold) {
    acecode::image::ImageNormalizeOptions options;
    options.force_png = true;
    const auto result = acecode::image::normalize_image_bytes(ppm_image_4x2(), "", options);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.changed);
    EXPECT_EQ(result.mime_type, "image/png");
    EXPECT_EQ(result.bytes.substr(0, 8), std::string("\x89PNG\r\n\x1a\n", 8));
    EXPECT_EQ(result.output.width, 4);
    EXPECT_EQ(result.output.height, 2);
}

TEST(ImageProcessor, ForcedPngResizesThumbnailsAndRejectsTruncatedImages) {
    acecode::image::ImageNormalizeOptions options;
    options.force_png = true;
    options.max_edge = 2;
    const auto result = acecode::image::normalize_image_bytes(ppm_image_4x2(), "", options);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.output.width, 2);
    EXPECT_EQ(result.output.height, 1);
    EXPECT_EQ(result.mime_type, "image/png");
    EXPECT_FALSE(acecode::image::normalize_image_bytes(result.bytes.substr(0, 28), "", options).ok);
}

TEST(ImageProcessor, ExactlyTenMiBAttemptsNormalization) {
    std::string bytes(acecode::image::kImageCompressionThresholdBytes, 'x');
    auto result = acecode::image::normalize_image_bytes(bytes, "image/png");
    EXPECT_TRUE(result.attempted);
    EXPECT_FALSE(result.ok);
}

namespace {

// 4x2 的 PPM,每个像素颜色都不同,便于核对截取后像素是否来自原图的对应位置。
std::string ppm_unique_pixels_4x2() {
    std::string bytes = "P6\n4 2\n255\n";
    for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<char>(10 + i * 30));
        bytes.push_back(static_cast<char>(200 - i * 20));
        bytes.push_back(static_cast<char>(i * 7));
    }
    return bytes;
}

std::string pixel_png(const std::string& image, int x, int y) {
    const auto pixel = acecode::image::crop_image_bytes(image, {x, y, 1, 1});
    EXPECT_TRUE(pixel.ok) << pixel.error;
    return pixel.bytes;
}

} // namespace

// 场景:从 4x2 的图里截 (1,0) 起 2x2 的区域。
// 期望:输出 2x2 的 PNG,截出图里每个像素与原图 (x+1, y) 处的像素一致(同一像素编码成
// 1x1 PNG 的字节相同,以此比较像素),而与相邻像素不同。
TEST(ImageProcessor, CropCutsTheRegionAndKeepsItsPixels) {
    const std::string image = ppm_unique_pixels_4x2();
    const auto cropped = acecode::image::crop_image_bytes(image, {1, 0, 2, 2});
    ASSERT_TRUE(cropped.ok) << cropped.error;
    EXPECT_EQ(cropped.mime_type, "image/png");
    EXPECT_EQ(cropped.output.width, 2);
    EXPECT_EQ(cropped.output.height, 2);
    EXPECT_EQ(cropped.input.width, 4);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            EXPECT_EQ(pixel_png(cropped.bytes, x, y), pixel_png(image, x + 1, y)) << x << "," << y;
        }
    }
    EXPECT_NE(pixel_png(cropped.bytes, 0, 0), pixel_png(image, 0, 0));
}

// 场景:区域一部分在图外、整个在图外、宽为 0。
// 期望:部分越界时裁到图内(region 报实际截取的范围);完全在图外或尺寸不为正时报错,
// 错误里带原图尺寸,模型可以据此改坐标重试。
TEST(ImageProcessor, CropClampsToTheImageAndRejectsRegionsThatMissIt) {
    const std::string image = ppm_unique_pixels_4x2();
    const auto clamped = acecode::image::crop_image_bytes(image, {-5, 1, 100, 100});
    ASSERT_TRUE(clamped.ok) << clamped.error;
    EXPECT_EQ(clamped.region.x, 0);
    EXPECT_EQ(clamped.region.y, 1);
    EXPECT_EQ(clamped.output.width, 4);
    EXPECT_EQ(clamped.output.height, 1);

    const auto outside = acecode::image::crop_image_bytes(image, {4, 0, 2, 2});
    EXPECT_FALSE(outside.ok);
    EXPECT_NE(outside.error.find("4x2"), std::string::npos);
    EXPECT_FALSE(acecode::image::crop_image_bytes(image, {0, 0, 0, 1}).ok);
    EXPECT_FALSE(acecode::image::crop_image_bytes("not an image", {0, 0, 1, 1}).ok);
}

// 场景:不透明的截图截出来后 PNG 超过上限(这里把上限设成 1 字节)。
// 期望:改用 JPEG 输出,尺寸不变;带透明度的图不会改成 JPEG(见上面 PNG 用例)。
TEST(ImageProcessor, OpaqueCropFallsBackToJpegWhenPngIsTooLarge) {
    const auto cropped = acecode::image::crop_image_bytes(ppm_unique_pixels_4x2(), {0, 0, 4, 2}, 1);
    ASSERT_TRUE(cropped.ok) << cropped.error;
    EXPECT_EQ(cropped.mime_type, "image/jpeg");
    EXPECT_EQ(cropped.output.width, 4);
    EXPECT_EQ(cropped.output.height, 2);
}

// 场景:按文件头识别图片类型(IM 平台给的类型不可信时用)。
// 期望:PNG / JPEG / GIF / WebP / BMP 各自识别;文字、过短的数据、RIFF 但不是 WebP 的都返回空。
TEST(ImageProcessor, ImageMimeFromBytesReadsTheSignature) {
    using acecode::image::image_mime_from_bytes;
    EXPECT_EQ(image_mime_from_bytes(std::string("\x89PNG\r\n\x1a\n", 8) + "rest"), "image/png");
    EXPECT_EQ(image_mime_from_bytes(std::string("\xFF\xD8\xFF\xE0", 4)), "image/jpeg");
    EXPECT_EQ(image_mime_from_bytes("GIF89a......"), "image/gif");
    EXPECT_EQ(image_mime_from_bytes(std::string("RIFF\x10\x00\x00\x00WEBPVP8 ", 16)), "image/webp");
    EXPECT_EQ(image_mime_from_bytes("BM" + std::string(30, '\0')), "image/bmp");
    EXPECT_EQ(image_mime_from_bytes(std::string("RIFF\x10\x00\x00\x00WAVEfmt ", 16)), "");
    EXPECT_EQ(image_mime_from_bytes("hello"), "");
    EXPECT_EQ(image_mime_from_bytes(""), "");
}
