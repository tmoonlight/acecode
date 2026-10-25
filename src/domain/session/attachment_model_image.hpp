#pragma once

#include "attachment_store.hpp"
#include "image/image_processor.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace acecode {

// How an image attachment is sent when "压缩图片" is on. The stored snapshot is
// never modified: the compressed form lives in memory and in a sidecar next to
// the snapshot (<snapshot>.model-v1.json / .bin), so a restart reuses it and
// turning the switch off sends the original again.
struct AttachmentModelImage {
    bool compressed = false;
    std::string bytes;      // only when compressed
    std::string mime_type;  // of bytes when compressed, else of the snapshot
    image::ImageInfo original;
    image::ImageInfo output;
    std::uintmax_t original_bytes = 0;
};

// Computer Use screenshots map clicks through their pixel size, so they are
// re-encoded at their own size instead of being downscaled.
bool attachment_image_keeps_dimensions(const AttachmentRecord& record);

// Compressed form of an image attachment, computed once per stored file.
// Returns nullptr when the snapshot cannot be read.
std::shared_ptr<const AttachmentModelImage> attachment_image_for_model(
    const AttachmentRecord& record,
    std::string* error = nullptr);

// Data URL length of the image as it will be sent; used by request budgets.
std::size_t attachment_image_wire_bytes(const AttachmentRecord& record);

// "[Image compressed before sending: ...]" note placed before a compressed image.
std::string attachment_model_image_note(const AttachmentModelImage& image);

void clear_attachment_model_image_cache_for_tests();

} // namespace acecode
