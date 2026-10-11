#include "static_assets.hpp"

#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <zlib.h>

namespace acecode::web {

namespace fs = std::filesystem;

namespace {

// 后缀 → content-type 映射。覆盖前端会用到的全部扩展名。未列出 → octet-stream。
struct MimeRow {
    const char* ext;
    const char* mime;
};

constexpr MimeRow kMimeTable[] = {
    {".html", "text/html; charset=utf-8"},
    {".htm",  "text/html; charset=utf-8"},
    {".css",  "text/css; charset=utf-8"},
    {".js",   "application/javascript; charset=utf-8"},
    {".mjs",  "application/javascript; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".map",  "application/json; charset=utf-8"},
    {".svg",  "image/svg+xml"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".ico",  "image/x-icon"},
    {".woff2","font/woff2"},
    {".woff", "font/woff"},
    {".ttf",  "font/ttf"},
    {".otf",  "font/otf"},
    {".md",   "text/markdown; charset=utf-8"},
    {".txt",  "text/plain; charset=utf-8"},
};

// 简易 ends_with(C++17 没 string.ends_with)
bool ends_with(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

bool ascii_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool positive_quality(std::string_view value) {
    value = trim(value);
    if (value.empty() || (value[0] != '0' && value[0] != '1')) return false;
    bool positive = value[0] == '1';
    if (value.size() == 1) return positive;
    if (value[1] != '.' || value.size() > 5) return false;
    for (std::size_t i = 2; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9' || (value[0] == '1' && value[i] != '0')) return false;
        positive = positive || value[i] != '0';
    }
    return positive;
}

} // namespace

bool accepts_gzip(std::string_view header) {
    std::optional<bool> gzip, wildcard;
    while (!header.empty()) {
        const auto comma = header.find(',');
        auto item = trim(header.substr(0, comma));
        const auto semi = item.find(';');
        const auto name = trim(item.substr(0, semi));
        bool allowed = true;
        if (semi != std::string_view::npos) {
            auto parameters = item.substr(semi + 1);
            while (!parameters.empty()) {
                const auto next = parameters.find(';');
                const auto parameter = trim(parameters.substr(0, next));
                const auto equals = parameter.find('=');
                if (ascii_equal(trim(parameter.substr(0, equals)), "q")) {
                    allowed = allowed && equals != std::string_view::npos &&
                              positive_quality(parameter.substr(equals + 1));
                }
                if (next == std::string_view::npos) break;
                parameters.remove_prefix(next + 1);
            }
        }
        if (ascii_equal(name, "gzip")) gzip = allowed && gzip.value_or(true);
        else if (name == "*") wildcard = allowed && wildcard.value_or(true);
        if (comma == std::string_view::npos) break;
        header.remove_prefix(comma + 1);
    }
    return gzip.value_or(wildcard.value_or(false));
}

std::string mime_type_for_path(const std::string& path) {
    for (const auto& row : kMimeTable) {
        if (ends_with(path, row.ext)) return row.mime;
    }
    return "application/octet-stream";
}

std::optional<AssetLookupResult> EmbeddedAssetSource::lookup(const std::string& path,
                                                           bool accept_gzip) const {
    const auto& m = embedded_asset_map();
    auto it = m.find(path);
    if (it == m.end()) return std::nullopt;
    AssetLookupResult r;
    r.content_type = mime_type_for_path(path);
    const auto& asset = it->second;
    r.vary_accept_encoding = asset.gzip;
    r.gzip = asset.gzip && accept_gzip;
    r.data = asset.data;
    r.size = asset.size;
    if (asset.gzip && !accept_gzip) {
        std::lock_guard<std::mutex> lock(decoded_mutex_);
        auto cached = decoded_.find(path);
        if (cached == decoded_.end()) {
            if (asset.size > std::numeric_limits<uInt>::max() ||
                asset.decoded_size >= std::numeric_limits<uInt>::max()) {
                throw std::runtime_error("embedded asset exceeds decompression limit");
            }
            auto decoded = std::make_shared<std::string>(asset.decoded_size + 1, '\0');
            z_stream stream{};
            stream.next_in = const_cast<Bytef*>(asset.data);
            stream.avail_in = static_cast<uInt>(asset.size);
            stream.next_out = reinterpret_cast<Bytef*>(decoded->data());
            stream.avail_out = static_cast<uInt>(decoded->size());
            if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
                throw std::runtime_error("cannot initialize embedded asset decompression");
            }
            // Own zlib's internal allocation; the stream itself has stack storage.
            const auto release = [](z_stream* state) { inflateEnd(state); };
            const std::unique_ptr<z_stream, decltype(release)> cleanup(&stream, release);
            const int result = inflate(&stream, Z_FINISH);
            const bool valid = result == Z_STREAM_END && stream.total_in == asset.size &&
                               stream.total_out == asset.decoded_size;
            if (!valid) throw std::runtime_error("corrupt compressed embedded asset: " + path);
            decoded->resize(asset.decoded_size);
            cached = decoded_.emplace(path, std::move(decoded)).first;
        }
        r.owned_buffer = cached->second;
        r.data = reinterpret_cast<const unsigned char*>(r.owned_buffer->data());
        r.size = r.owned_buffer->size();
    }
    return r;
}

std::optional<AssetLookupResult> FileSystemAssetSource::lookup(const std::string& path,
                                                             bool /*accept_gzip*/) const {
    fs::path full = path_from_utf8(root_) / path_from_utf8(path);

    // 安全:确保 full 在 root_ 之下(防 ".." 穿越)。weakly_canonical 可能解析失败,
    // 用 lexically_normal 也行。
    std::error_code ec;
    auto canon = fs::weakly_canonical(full, ec);
    if (ec) return std::nullopt;
    auto root_canon = fs::weakly_canonical(path_from_utf8(root_), ec);
    if (ec) return std::nullopt;
    auto rel = path_to_utf8_generic(canon.lexically_relative(root_canon));
    if (rel.rfind("..", 0) == 0) return std::nullopt; // 越界

    if (!fs::exists(canon, ec) || !fs::is_regular_file(canon, ec)) return std::nullopt;

    std::ifstream ifs(canon, std::ios::binary);
    if (!ifs) return std::nullopt;
    auto buf = std::make_shared<std::string>(
        (std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    AssetLookupResult r;
    r.content_type   = mime_type_for_path(path);
    r.data           = reinterpret_cast<const unsigned char*>(buf->data());
    r.size           = buf->size();
    r.owned_buffer   = buf;
    return r;
}

std::unique_ptr<AssetSource> make_asset_source(const std::string& static_dir) {
    if (static_dir.empty()) {
        return std::make_unique<EmbeddedAssetSource>();
    }
    if (!fs::exists(path_from_utf8(static_dir))) {
        throw std::runtime_error("web.static_dir path does not exist: " + static_dir);
    }
    LOG_INFO("[web] using FileSystem asset source: " + static_dir);
    return std::make_unique<FileSystemAssetSource>(static_dir);
}

} // namespace acecode::web
