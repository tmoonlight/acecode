#include "image_processor.hpp"

#include "utils/logger.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <sstream>
#include <utility>
#include <vector>

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

namespace acecode::image {

namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

void set_error(std::string* error, std::string value) {
    if (error) *error = std::move(value);
}

bool has_real_alpha(const unsigned char* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) return false;
    const std::size_t count = static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < count; ++i) {
        if (pixels[i * 4u + 3u] != 255) return true;
    }
    return false;
}

void write_bytes_callback(void* context, void* data, int size) {
    if (!context || !data || size <= 0) return;
    auto* out = static_cast<std::string*>(context);
    out->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

std::string encode_png(const unsigned char* pixels, int width, int height) {
    std::string out;
    const int ok = stbi_write_png_to_func(
        write_bytes_callback,
        &out,
        width,
        height,
        4,
        pixels,
        width * 4);
    if (!ok) out.clear();
    return out;
}

std::string encode_jpeg(const unsigned char* pixels,
                        int width,
                        int height,
                        int quality,
                        std::size_t target_bytes) {
    const int qualities[] = {
        quality,
        std::min(quality, 80),
        std::min(quality, 75),
        std::min(quality, 70),
    };
    std::string best;
    for (int q : qualities) {
        std::string out;
        const int ok = stbi_write_jpg_to_func(
            write_bytes_callback,
            &out,
            width,
            height,
            4,
            pixels,
            q);
        if (!ok || out.empty()) continue;
        if (best.empty() || out.size() < best.size()) best = out;
        if (out.size() <= target_bytes) return out;
    }
    return best;
}

std::string summarize_dimensions(const ImageInfo& info) {
    if (info.width <= 0 || info.height <= 0) return "unknown";
    std::ostringstream oss;
    oss << info.width << "x" << info.height << " channels=" << info.channels;
    return oss.str();
}

std::atomic<bool> g_model_image_compression_enabled{true};

std::string encode_jpeg_at_quality(const unsigned char* pixels,
                                   int width,
                                   int height,
                                   int quality) {
    std::string out;
    const int ok = stbi_write_jpg_to_func(
        write_bytes_callback, &out, width, height, 4, pixels, quality);
    if (!ok) out.clear();
    return out;
}

// Scales the long side to `side` and rounds both sides like grok-build's resize
// (the image crate's resize_dimensions), never below one pixel.
std::pair<int, int> fit_long_side(int width, int height, int side) {
    const int long_side = std::max(width, height);
    if (side >= long_side) return {width, height};
    const double ratio = static_cast<double>(side) / static_cast<double>(long_side);
    return {std::max(1, static_cast<int>(std::lround(width * ratio))),
            std::max(1, static_cast<int>(std::lround(height * ratio)))};
}

// Largest long side whose rounded output stays within max_pixels.
int area_capped_side(int width, int height, std::uint64_t max_pixels) {
    const int long_side = std::max(width, height);
    const double scale = std::sqrt(static_cast<double>(max_pixels) /
                                   (static_cast<double>(width) * static_cast<double>(height)));
    int side = std::clamp(static_cast<int>(std::floor(long_side * scale)), 1, long_side);
    while (side > 1) {
        const auto [w, h] = fit_long_side(width, height, side);
        if (static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) <= max_pixels) break;
        --side;
    }
    return side;
}

} // namespace

bool model_image_compression_enabled() {
    return g_model_image_compression_enabled.load(std::memory_order_relaxed);
}

void set_model_image_compression_enabled(bool enabled) {
    g_model_image_compression_enabled.store(enabled, std::memory_order_relaxed);
}

ModelImageCompression compress_image_for_model(std::string_view bytes,
                                               std::string_view mime_type,
                                               bool keep_dimensions,
                                               const ModelImageLimits& limits) {
    ModelImageCompression result;
    if (!is_supported_raster_image_mime(mime_type)) {
        result.error = "unsupported image mime";
        return result;
    }
    std::string probe_error;
    const auto info = probe_image_info(bytes, &probe_error);
    if (!info.has_value()) {
        result.error = probe_error.empty() ? "unsupported image format" : probe_error;
        return result;
    }
    result.input = *info;
    result.output = *info;

    const std::uint64_t pixel_count = static_cast<std::uint64_t>(info->width) *
        static_cast<std::uint64_t>(info->height);
    const bool exceeded_size = bytes.size() > limits.max_bytes;
    const bool exceeded_dimensions = !keep_dimensions &&
        (info->width > limits.max_side || info->height > limits.max_side ||
         pixel_count > limits.max_pixels);
    if (!exceeded_size && !exceeded_dimensions) {
        result.outcome = ModelImageOutcome::Unchanged;
        return result;
    }
    if (pixel_count > kImageNormalizeMaxPixels) {
        result.error = "image pixel count exceeds normalization cap";
        return result;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()),
        &width,
        &height,
        &channels,
        4);
    if (!decoded || width <= 0 || height <= 0) {
        const char* reason = stbi_failure_reason();
        result.error = reason ? reason : "image decode failed";
        if (decoded) stbi_image_free(decoded);
        return result;
    }

    // JPEG would flatten real transparency, so such images only get PNG candidates.
    const bool alpha = has_real_alpha(decoded, width, height);
    const int long_side = std::max(width, height);
    int side = keep_dimensions ? long_side : std::min(limits.max_side, long_side);
    if (!keep_dimensions && pixel_count > limits.max_pixels) {
        side = std::min(side, area_capped_side(width, height, limits.max_pixels));
    }

    std::vector<unsigned char> resized;
    for (;;) {
        const auto [out_width, out_height] = fit_long_side(width, height, side);
        const unsigned char* pixels = decoded;
        if (out_width != width || out_height != height) {
            resized.resize(static_cast<std::size_t>(out_width) *
                           static_cast<std::size_t>(out_height) * 4u);
            if (!stbir_resize_uint8_srgb(decoded, width, height, 0, resized.data(),
                                         out_width, out_height, 0, STBIR_RGBA)) {
                stbi_image_free(decoded);
                result.error = "image resize failed";
                return result;
            }
            pixels = resized.data();
        }

        std::string png = encode_png(pixels, out_width, out_height);
        if (png.size() > limits.max_bytes) png.clear();
        std::string jpeg;
        if (!alpha) {
            for (int quality : limits.jpeg_quality_steps) {
                auto candidate = encode_jpeg_at_quality(pixels, out_width, out_height, quality);
                if (!candidate.empty() && candidate.size() <= limits.max_bytes) {
                    jpeg = std::move(candidate);
                    break;
                }
            }
        }

        if (!png.empty() || !jpeg.empty()) {
            const bool use_png = !png.empty() && (jpeg.empty() || png.size() <= jpeg.size());
            result.outcome = ModelImageOutcome::Compressed;
            result.bytes = use_png ? std::move(png) : std::move(jpeg);
            result.mime_type = use_png ? "image/png" : "image/jpeg";
            result.output = {out_width, out_height, 4, alpha && use_png};
            stbi_image_free(decoded);
            LOG_INFO("[image-compress] compressed"
                     " original_size=" + std::to_string(bytes.size()) +
                     " output_size=" + std::to_string(result.bytes.size()) +
                     " output_mime=" + result.mime_type +
                     " input_dims=" + summarize_dimensions(result.input) +
                     " output_dims=" + summarize_dimensions(result.output) +
                     " keep_dimensions=" + std::string(keep_dimensions ? "1" : "0"));
            return result;
        }
        if (keep_dimensions || side <= limits.min_side) {
            stbi_image_free(decoded);
            result.outcome = ModelImageOutcome::CouldNotFit;
            result.error = "could not fit under " + std::to_string(limits.max_bytes) +
                           " bytes (last long side " + std::to_string(side) + "px)";
            LOG_INFO("[image-compress] kept original reason=" + result.error +
                     " original_size=" + std::to_string(bytes.size()));
            return result;
        }
        side = side * 3 / 4;
    }
}

bool is_supported_raster_image_mime(std::string_view mime_type) {
    const std::string mime = ascii_lower(std::string(mime_type));
    if (mime.empty()) return true;
    if (mime == "image/svg+xml") return false;
    if (mime == "image/gif") return false; // Avoid flattening animated GIFs.
    if (mime == "image/webp") return false; // stb_image does not decode WebP.
    return mime.rfind("image/", 0) == 0;
}

std::optional<ImageInfo> probe_image_info(std::string_view bytes,
                                          std::string* error) {
    if (bytes.empty()) {
        set_error(error, "image data is empty");
        return std::nullopt;
    }
    if (bytes.size() > static_cast<std::size_t>(INT_MAX)) {
        set_error(error, "image data too large to decode");
        return std::nullopt;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    const int ok = stbi_info_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()),
        &width,
        &height,
        &channels);
    if (!ok || width <= 0 || height <= 0) {
        const char* reason = stbi_failure_reason();
        set_error(error, reason ? reason : "unsupported image format");
        return std::nullopt;
    }
    ImageInfo info;
    info.width = width;
    info.height = height;
    info.channels = channels;
    info.has_alpha = channels == 2 || channels == 4;
    return info;
}

ImageNormalizeResult normalize_image_bytes(
    std::string_view bytes,
    std::string_view mime_type,
    const ImageNormalizeOptions& options) {
    ImageNormalizeResult result;
    result.backend = "stb";
    result.mime_type = std::string(mime_type);

    if (!is_supported_raster_image_mime(mime_type)) {
        result.attempted = bytes.size() >= options.compression_threshold_bytes ||
            options.force;
        result.reason = "unsupported mime";
        result.error = "unsupported image mime for stb normalization";
        if (result.attempted) {
            LOG_WARN("[image-normalize] unsupported mime"
                     " size=" + std::to_string(bytes.size()) +
                     " mime=" + std::string(mime_type));
        }
        return result;
    }

    std::string probe_error;
    auto info = probe_image_info(bytes, &probe_error);
    if (!info.has_value()) {
        result.attempted = bytes.size() >= options.compression_threshold_bytes ||
            options.force;
        result.error = probe_error.empty() ? "unsupported image format" : probe_error;
        if (result.attempted) {
            LOG_WARN("[image-normalize] probe failed"
                     " size=" + std::to_string(bytes.size()) +
                     " mime=" + std::string(mime_type) +
                     " error=" + result.error);
        }
        return result;
    }
    result.input = *info;

    const int max_dim = std::max(info->width, info->height);
    const bool size_trigger = bytes.size() >= options.compression_threshold_bytes;
    const bool dimension_trigger = options.max_edge > 0 && max_dim > options.max_edge;
    if (!options.force && !options.force_png && !size_trigger && !dimension_trigger) {
        result.ok = true;
        result.reason = "below thresholds";
        result.output = result.input;
        return result;
    }

    result.attempted = true;

    const std::size_t pixel_count = static_cast<std::size_t>(info->width) *
        static_cast<std::size_t>(info->height);
    if (pixel_count > options.max_pixels) {
        result.error = "image pixel count exceeds normalization cap";
        LOG_WARN("[image-normalize] reject huge image"
                 " size=" + std::to_string(bytes.size()) +
                 " dims=" + summarize_dimensions(*info) +
                 " max_pixels=" + std::to_string(options.max_pixels));
        return result;
    }

    LOG_INFO("[image-normalize] start"
             " backend=stb"
             " size=" + std::to_string(bytes.size()) +
             " threshold=" + std::to_string(options.compression_threshold_bytes) +
             " mime=" + std::string(mime_type) +
             " dims=" + summarize_dimensions(*info) +
             " size_trigger=" + std::string(size_trigger ? "1" : "0") +
             " dimension_trigger=" + std::string(dimension_trigger ? "1" : "0"));

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()),
        &width,
        &height,
        &channels,
        4);
    if (!decoded || width <= 0 || height <= 0) {
        const char* reason = stbi_failure_reason();
        result.error = reason ? reason : "image decode failed";
        if (decoded) stbi_image_free(decoded);
        LOG_WARN("[image-normalize] decode failed error=" + result.error);
        return result;
    }

    const bool alpha = has_real_alpha(decoded, width, height);
    int out_width = width;
    int out_height = height;
    if (dimension_trigger) {
        const double scale = static_cast<double>(options.max_edge) /
            static_cast<double>(std::max(width, height));
        out_width = std::max(1, static_cast<int>(std::round(width * scale)));
        out_height = std::max(1, static_cast<int>(std::round(height * scale)));
    }

    std::vector<unsigned char> resized;
    const unsigned char* output_pixels = decoded;
    if (out_width != width || out_height != height) {
        resized.resize(static_cast<std::size_t>(out_width) *
                       static_cast<std::size_t>(out_height) * 4u);
        unsigned char* resize_ok = stbir_resize_uint8_srgb(
            decoded,
            width,
            height,
            0,
            resized.data(),
            out_width,
            out_height,
            0,
            STBIR_RGBA);
        if (!resize_ok) {
            stbi_image_free(decoded);
            result.error = "image resize failed";
            LOG_WARN("[image-normalize] resize failed");
            return result;
        }
        output_pixels = resized.data();
    }

    std::string out_mime;
    std::string encoded;
    if (alpha || options.force_png) {
        out_mime = "image/png";
        encoded = encode_png(output_pixels, out_width, out_height);
    } else {
        out_mime = "image/jpeg";
        encoded = encode_jpeg(output_pixels,
                              out_width,
                              out_height,
                              options.jpeg_quality,
                              options.compression_threshold_bytes);
    }
    stbi_image_free(decoded);

    if (encoded.empty()) {
        result.error = "image encode failed";
        LOG_WARN("[image-normalize] encode failed");
        return result;
    }
    if (encoded.size() > options.final_max_bytes) {
        result.error = "normalized image still exceeds hard cap";
        LOG_WARN("[image-normalize] normalized image too large"
                 " output_size=" + std::to_string(encoded.size()) +
                 " final_max=" + std::to_string(options.final_max_bytes));
        return result;
    }
    if (!options.force_png && !dimension_trigger &&
        encoded.size() >= bytes.size() &&
        bytes.size() <= options.final_max_bytes) {
        result.ok = true;
        result.changed = false;
        result.reason = "normalized output was not smaller";
        result.output = {out_width, out_height, 4, alpha};
        LOG_INFO("[image-normalize] keep original"
                 " reason=encoded-not-smaller"
                 " original_size=" + std::to_string(bytes.size()) +
                 " encoded_size=" + std::to_string(encoded.size()) +
                 " output_dims=" + summarize_dimensions(result.output));
        return result;
    }

    result.ok = true;
    result.changed = true;
    result.bytes = std::move(encoded);
    result.mime_type = out_mime;
    result.output = {out_width, out_height, 4, alpha};
    result.reason = size_trigger && dimension_trigger
        ? "size and dimensions exceeded thresholds"
        : (size_trigger ? "size exceeded threshold" : "dimensions exceeded threshold");

    LOG_INFO("[image-normalize] changed"
             " backend=stb"
             " original_size=" + std::to_string(bytes.size()) +
             " output_size=" + std::to_string(result.bytes.size()) +
             " original_mime=" + std::string(mime_type) +
             " output_mime=" + result.mime_type +
             " input_dims=" + summarize_dimensions(result.input) +
             " output_dims=" + summarize_dimensions(result.output));
    return result;
}

std::string image_mime_from_bytes(std::string_view bytes) {
    const auto starts = [&](std::string_view prefix, std::size_t offset = 0) {
        return bytes.size() >= offset + prefix.size() && bytes.substr(offset, prefix.size()) == prefix;
    };
    if (starts(std::string_view("\x89PNG\r\n\x1a\n", 8))) return "image/png";
    if (starts(std::string_view("\xFF\xD8\xFF", 3))) return "image/jpeg";
    if (starts("GIF87a") || starts("GIF89a")) return "image/gif";
    if (starts("RIFF") && starts("WEBP", 8)) return "image/webp";
    if (starts("BM") && bytes.size() >= 26) return "image/bmp";
    return {};
}

ImageCropResult crop_image_bytes(std::string_view bytes,
                                 const ImageCropRegion& region,
                                 std::size_t max_png_bytes) {
    ImageCropResult result;
    if (region.width <= 0 || region.height <= 0) {
        result.error = "crop width and height must be positive";
        return result;
    }
    auto info = probe_image_info(bytes, &result.error);
    if (!info.has_value()) return result;
    result.input = *info;
    if (static_cast<std::uint64_t>(info->width) * static_cast<std::uint64_t>(info->height) >
        kImageNormalizeMaxPixels) {
        result.error = "image is too large to crop";
        return result;
    }

    // Clamp in 64-bit so a huge width or a negative origin cannot overflow.
    const std::int64_t left = std::max<std::int64_t>(0, region.x);
    const std::int64_t top = std::max<std::int64_t>(0, region.y);
    const std::int64_t right =
        std::min<std::int64_t>(info->width, static_cast<std::int64_t>(region.x) + region.width);
    const std::int64_t bottom =
        std::min<std::int64_t>(info->height, static_cast<std::int64_t>(region.y) + region.height);
    if (left >= right || top >= bottom) {
        result.error = "crop region is outside the " + std::to_string(info->width) + "x" +
                       std::to_string(info->height) + " image";
        return result;
    }
    result.region = {static_cast<int>(left), static_cast<int>(top),
                     static_cast<int>(right - left), static_cast<int>(bottom - top)};

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                             static_cast<int>(bytes.size()), &width, &height,
                                             &channels, 4);
    if (!decoded || width != info->width || height != info->height) {
        const char* reason = stbi_failure_reason();
        result.error = reason ? reason : "image decode failed";
        if (decoded) stbi_image_free(decoded);
        return result;
    }
    const int out_width = result.region.width;
    const int out_height = result.region.height;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(out_width) *
                                      static_cast<std::size_t>(out_height) * 4u);
    for (int row = 0; row < out_height; ++row) {
        const auto* src = decoded + ((static_cast<std::size_t>(result.region.y + row) *
                                      static_cast<std::size_t>(width)) +
                                     static_cast<std::size_t>(result.region.x)) * 4u;
        std::copy(src, src + static_cast<std::size_t>(out_width) * 4u,
                  pixels.begin() + static_cast<std::ptrdiff_t>(
                                       static_cast<std::size_t>(row) *
                                       static_cast<std::size_t>(out_width) * 4u));
    }
    stbi_image_free(decoded);

    const bool alpha = has_real_alpha(pixels.data(), out_width, out_height);
    std::string encoded = encode_png(pixels.data(), out_width, out_height);
    std::string mime = "image/png";
    if (!alpha && (encoded.empty() || encoded.size() > max_png_bytes)) {
        std::string jpeg = encode_jpeg(pixels.data(), out_width, out_height, 90, max_png_bytes);
        if (!jpeg.empty()) {
            encoded = std::move(jpeg);
            mime = "image/jpeg";
        }
    }
    if (encoded.empty()) {
        result.error = "image encode failed";
        return result;
    }
    result.ok = true;
    result.bytes = std::move(encoded);
    result.mime_type = std::move(mime);
    result.output = {out_width, out_height, 4, alpha};
    return result;
}

} // namespace acecode::image
