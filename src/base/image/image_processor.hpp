#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acecode::image {

inline constexpr std::size_t kImageCompressionThresholdBytes = 10u * 1024u * 1024u;
inline constexpr int kImageNormalizeMaxEdge = 2560;
inline constexpr std::size_t kImageNormalizeFinalMaxBytes = 25u * 1024u * 1024u;
inline constexpr std::size_t kImageNormalizeMaxPixels = 80u * 1000u * 1000u;

struct ImageInfo {
    int width = 0;
    int height = 0;
    int channels = 0;
    bool has_alpha = false;
};

struct ImageNormalizeOptions {
    std::size_t compression_threshold_bytes = kImageCompressionThresholdBytes;
    std::size_t final_max_bytes = kImageNormalizeFinalMaxBytes;
    std::size_t max_pixels = kImageNormalizeMaxPixels;
    int max_edge = kImageNormalizeMaxEdge;
    int jpeg_quality = 85;
    bool force = false;
    // Data packages such as themes require PNG even for opaque images.
    bool force_png = false;
};

struct ImageNormalizeResult {
    bool attempted = false;
    bool ok = false;
    bool changed = false;
    std::string bytes;
    std::string mime_type;
    ImageInfo input;
    ImageInfo output;
    std::string backend;
    std::string reason;
    std::string error;
};

bool is_supported_raster_image_mime(std::string_view mime_type);

std::optional<ImageInfo> probe_image_info(std::string_view bytes,
                                          std::string* error = nullptr);

ImageNormalizeResult normalize_image_bytes(
    std::string_view bytes,
    std::string_view mime_type,
    const ImageNormalizeOptions& options = {});

// Limits for images sent to a model, taken from grok-build's attachment
// normalizer (xai-grok-shell image_normalize.rs NORMALIZE_PARAMS). A 1.5 MB
// image is about 2 MB once base64 encoded; max_pixels mirrors the size the
// model side downsamples to anyway, so larger images only waste bytes.
struct ModelImageLimits {
    std::size_t max_bytes = 1'500'000;
    int max_side = 2000;
    std::uint64_t max_pixels = 2'408'448;
    int min_side = 512;
    std::vector<int> jpeg_quality_steps = {88, 80, 72, 64, 56, 48, 40, 32};
};

enum class ModelImageOutcome {
    Unchanged,    // already within every limit
    Compressed,   // bytes/mime_type/output hold the re-encoded image
    CouldNotFit,  // no candidate fit before reaching min_side; keep the original
    Unsupported,  // not decodable here; callers fall back to normalize_image_bytes
};

struct ModelImageCompression {
    ModelImageOutcome outcome = ModelImageOutcome::Unsupported;
    std::string bytes;
    std::string mime_type;
    ImageInfo input;
    ImageInfo output;
    std::string error;
};

// Re-encodes an image so it fits the limits: PNG and JPEG candidates are tried
// at the current size, the smaller one that fits wins, otherwise the long side
// shrinks by a quarter and the attempt repeats. Images are never enlarged.
// keep_dimensions is for screenshots whose coordinates depend on their pixel
// size: only the byte limit applies and the size never changes.
ModelImageCompression compress_image_for_model(
    std::string_view bytes,
    std::string_view mime_type,
    bool keep_dimensions = false,
    const ModelImageLimits& limits = {});

// Process-wide "压缩图片" switch (config image_compression.enabled). Read on
// every request so a settings change applies to the next model call.
bool model_image_compression_enabled();
void set_model_image_compression_enabled(bool enabled);

} // namespace acecode::image
