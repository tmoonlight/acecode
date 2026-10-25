#include "attachment_model_image.hpp"

#include "utils/atomic_file.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <list>
#include <mutex>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace acecode {

namespace {

// Bump when ModelImageLimits change so old sidecars are ignored.
constexpr int kSidecarVersion = 1;
constexpr const char* kSidecarSuffix = ".model-v1";
// Same budget as grok-build's process-wide normalize cache.
constexpr std::size_t kCacheMaxBytes = 64u * 1024u * 1024u;
constexpr std::size_t kCacheMaxEntries = 4096;
constexpr std::size_t kDataUrlPrefixBytes = 32;

using ModelImagePtr = std::shared_ptr<const AttachmentModelImage>;

struct ModelImageCache {
    std::mutex mu;
    std::list<std::pair<std::string, ModelImagePtr>> lru;
    std::unordered_map<std::string, std::list<std::pair<std::string, ModelImagePtr>>::iterator> index;
    std::size_t bytes = 0;
};

ModelImageCache& model_image_cache() {
    static ModelImageCache cache;
    return cache;
}

ModelImagePtr cache_get(const std::string& key) {
    auto& cache = model_image_cache();
    std::lock_guard<std::mutex> lock(cache.mu);
    const auto found = cache.index.find(key);
    if (found == cache.index.end()) return nullptr;
    cache.lru.splice(cache.lru.begin(), cache.lru, found->second);
    return found->second->second;
}

void cache_put(const std::string& key, ModelImagePtr value) {
    auto& cache = model_image_cache();
    std::lock_guard<std::mutex> lock(cache.mu);
    if (const auto found = cache.index.find(key); found != cache.index.end()) {
        cache.bytes -= found->second->second->bytes.size();
        cache.lru.erase(found->second);
        cache.index.erase(found);
    }
    if (value->bytes.size() > kCacheMaxBytes) return;
    cache.bytes += value->bytes.size();
    cache.lru.emplace_front(key, std::move(value));
    cache.index[key] = cache.lru.begin();
    while (!cache.lru.empty() &&
           (cache.bytes > kCacheMaxBytes || cache.lru.size() > kCacheMaxEntries)) {
        cache.bytes -= cache.lru.back().second->bytes.size();
        cache.index.erase(cache.lru.back().first);
        cache.lru.pop_back();
    }
}

fs::path sidecar_path(const fs::path& source, const char* extension) {
    fs::path path = source;
    path += kSidecarSuffix;
    path += extension;
    return path;
}

std::optional<std::string> read_whole_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

nlohmann::json image_info_json(const image::ImageInfo& info) {
    return {{"width", info.width}, {"height", info.height}};
}

image::ImageInfo image_info_from_json(const nlohmann::json& j) {
    image::ImageInfo info;
    if (!j.is_object()) return info;
    info.width = j.value("width", 0);
    info.height = j.value("height", 0);
    return info;
}

// The snapshot never changes after it is saved, so its size is enough to tell
// whether a sidecar still belongs to it.
ModelImagePtr read_sidecar(const fs::path& source, std::uintmax_t source_size) {
    const auto text = read_whole_file(sidecar_path(source, ".json"));
    if (!text) return nullptr;
    const auto j = nlohmann::json::parse(*text, nullptr, false);
    if (!j.is_object() || j.value("version", 0) != kSidecarVersion ||
        j.value("source_size", std::uintmax_t{0}) != source_size) {
        return nullptr;
    }
    auto value = std::make_shared<AttachmentModelImage>();
    value->compressed = j.value("compressed", false);
    value->mime_type = j.value("mime_type", std::string{});
    value->original = image_info_from_json(j.value("original", nlohmann::json::object()));
    value->output = image_info_from_json(j.value("output", nlohmann::json::object()));
    value->original_bytes = source_size;
    if (value->compressed) {
        auto bytes = read_whole_file(sidecar_path(source, ".bin"));
        if (!bytes || bytes->size() != j.value("bytes", std::size_t{0}) ||
            value->mime_type.empty()) {
            return nullptr;
        }
        value->bytes = std::move(*bytes);
    }
    return value;
}

void write_sidecar(const fs::path& source, std::uintmax_t source_size,
                   const AttachmentModelImage& value) {
    if (value.compressed &&
        !atomic_write_file(path_to_utf8(sidecar_path(source, ".bin")), value.bytes)) {
        LOG_WARN("[image-compress] failed to write sidecar for " + path_to_utf8(source));
        return;
    }
    const nlohmann::json j = {
        {"version", kSidecarVersion},
        {"source_size", source_size},
        {"compressed", value.compressed},
        {"mime_type", value.mime_type},
        {"bytes", value.bytes.size()},
        {"original", image_info_json(value.original)},
        {"output", image_info_json(value.output)},
    };
    if (!atomic_write_file(path_to_utf8(sidecar_path(source, ".json")), j.dump())) {
        LOG_WARN("[image-compress] failed to write sidecar for " + path_to_utf8(source));
    }
}

std::string format_byte_size(std::uintmax_t bytes) {
    char buffer[32];
    if (bytes >= 1024u * 1024u) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024u) {
        std::snprintf(buffer, sizeof(buffer), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

} // namespace

bool attachment_image_keeps_dimensions(const AttachmentRecord& record) {
    return record.metadata.is_object() && record.metadata.contains("computer_use");
}

std::shared_ptr<const AttachmentModelImage> attachment_image_for_model(
    const AttachmentRecord& record,
    std::string* error) {
    if (record.path.empty()) {
        if (error) *error = "attachment path missing";
        return nullptr;
    }
    const fs::path source = path_from_utf8(record.path);
    std::error_code ec;
    const std::uintmax_t source_size = fs::file_size(source, ec);
    if (ec) {
        if (error) *error = "failed to open attachment";
        return nullptr;
    }
    const bool keep_dimensions = attachment_image_keeps_dimensions(record);
    const std::string key = path_to_utf8(source) + "|" + std::to_string(source_size) +
                            (keep_dimensions ? "|keep" : "|fit");
    if (auto cached = cache_get(key)) return cached;
    if (auto stored = read_sidecar(source, source_size)) {
        cache_put(key, stored);
        return stored;
    }

    auto bytes = read_attachment_bytes(record, kMaxAttachmentBytes, error);
    if (!bytes.has_value()) return nullptr;
    const std::string mime = attachment_mime_for_name(record.name, record.mime_type);
    auto compression = image::compress_image_for_model(*bytes, mime, keep_dimensions);
    auto value = std::make_shared<AttachmentModelImage>();
    value->original = compression.input;
    value->output = compression.output;
    value->original_bytes = bytes->size();
    value->mime_type = mime;
    if (compression.outcome == image::ModelImageOutcome::Compressed) {
        value->compressed = true;
        value->bytes = std::move(compression.bytes);
        value->mime_type = std::move(compression.mime_type);
    }
    write_sidecar(source, source_size, *value);
    cache_put(key, value);
    return value;
}

std::size_t attachment_image_wire_bytes(const AttachmentRecord& record) {
    const auto image = attachment_image_for_model(record);
    std::uintmax_t raw = record.size_bytes;
    if (image) raw = image->compressed ? image->bytes.size() : image->original_bytes;
    return static_cast<std::size_t>(4u * ((raw + 2u) / 3u)) + kDataUrlPrefixBytes;
}

std::string attachment_model_image_note(const AttachmentModelImage& image) {
    return "[Image compressed before sending: " +
           std::to_string(image.original.width) + "x" + std::to_string(image.original.height) +
           ", " + format_byte_size(image.original_bytes) + " -> " +
           std::to_string(image.output.width) + "x" + std::to_string(image.output.height) +
           ", " + format_byte_size(image.bytes.size()) +
           ". Fine details may have been lost.]";
}

void clear_attachment_model_image_cache_for_tests() {
    auto& cache = model_image_cache();
    std::lock_guard<std::mutex> lock(cache.mu);
    cache.lru.clear();
    cache.index.clear();
    cache.bytes = 0;
}

} // namespace acecode
