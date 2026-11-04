#include "payload_archive.hpp"

#include "utils/encoding.hpp"
#include "utils/sha256.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>

namespace acecode::installer {
namespace {

namespace fs = std::filesystem;

// 解压出来的总大小上限:当前包约 60 MB,留足余量又能挡住压缩炸弹。
constexpr std::uint64_t kMaxExtractedBytes = 4ull * 1024 * 1024 * 1024;
constexpr std::size_t kCopyChunk = 256 * 1024;

bool is_hex64(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return std::isdigit(c) || (c >= 'a' && c <= 'f');
           });
}

bool is_known_target(const std::string& target) {
    return target == "windows-x64" || target == "windows-arm64";
}

struct ZipCloser {
    void operator()(zip_t* archive) const {
        if (archive) zip_discard(archive);
    }
};
struct ZipFileCloser {
    void operator()(zip_file_t* file) const {
        if (file) zip_fclose(file);
    }
};

bool is_reserved_device_name(std::string part) {
    const auto dot = part.find('.');
    if (dot != std::string::npos) part = part.substr(0, dot);
    std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    static const std::array<const char*, 4> kNames{"CON", "PRN", "AUX", "NUL"};
    if (std::find_if(kNames.begin(), kNames.end(),
                     [&part](const char* name) { return part == name; }) != kNames.end()) {
        return true;
    }
    return part.size() == 4 && (part.rfind("COM", 0) == 0 || part.rfind("LPT", 0) == 0) &&
           part[3] >= '1' && part[3] <= '9';
}

}  // namespace

std::optional<PayloadInfo> parse_payload_info(const std::string& json_text, std::string* error) {
    const auto json = nlohmann::json::parse(json_text, nullptr, false);
    const auto fail = [error](const char* message) -> std::optional<PayloadInfo> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (json.is_discarded() || !json.is_object()) return fail("payload info is not a JSON object");
    PayloadInfo info;
    info.schema = json.value("schema", 0);
    if (info.schema != 1) return fail("unsupported payload info schema");
    info.version = json.value("version", std::string());
    info.target = json.value("target", std::string());
    info.payload_sha256 = json.value("payload_sha256", std::string());
    info.uninstaller_sha256 = json.value("uninstaller_sha256", std::string());
    const auto size = json.find("payload_size");
    if (size != json.end() && size->is_number_unsigned()) {
        info.payload_size = size->get<std::uint64_t>();
    }
    if (info.version.empty() ||
        !std::all_of(info.version.begin(), info.version.end(), [](unsigned char c) {
            return std::isdigit(c) || c == '.';
        })) {
        return fail("payload info has an invalid version");
    }
    if (!is_known_target(info.target)) return fail("payload info has an unknown target");
    if (!is_hex64(info.payload_sha256) || !is_hex64(info.uninstaller_sha256)) {
        return fail("payload info has an invalid sha256");
    }
    if (info.payload_size == 0) return fail("payload info has no payload size");
    return info;
}

std::string serialize_payload_info(const PayloadInfo& info) {
    nlohmann::ordered_json json;
    json["schema"] = info.schema;
    json["version"] = info.version;
    json["target"] = info.target;
    json["payload_sha256"] = info.payload_sha256;
    json["payload_size"] = info.payload_size;
    json["uninstaller_sha256"] = info.uninstaller_sha256;
    return json.dump();
}

bool is_safe_archive_entry(const std::string& raw) {
    std::string name = raw;
    std::replace(name.begin(), name.end(), '\\', '/');
    while (!name.empty() && name.back() == '/') name.pop_back();
    if (name.empty() || name.front() == '/') return false;
    if (name.find(':') != std::string::npos) return false;
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto end = name.find('/', start);
        const std::string part = name.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (part.back() == '.' || part.back() == ' ') return false;
        if (is_reserved_device_name(part)) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

std::string sha256_of(std::string_view bytes) {
    Sha256 sha;
    sha.update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    return sha.final_hex();
}

ExtractResult extract_archive(std::string_view zip_bytes,
                              const fs::path& destination,
                              ExtractObserver* observer) {
    ExtractResult result;
    zip_error_t zip_error;
    zip_error_init(&zip_error);
    zip_source_t* source = zip_source_buffer_create(zip_bytes.data(), zip_bytes.size(), 0, &zip_error);
    if (!source) {
        result.error = "failed to read the installer payload";
        zip_error_fini(&zip_error);
        return result;
    }
    std::unique_ptr<zip_t, ZipCloser> archive(zip_open_from_source(source, ZIP_RDONLY, &zip_error));
    if (!archive) {
        zip_source_free(source);
        result.error = "installer payload is not a valid zip archive";
        zip_error_fini(&zip_error);
        return result;
    }
    zip_error_fini(&zip_error);

    const zip_int64_t count = zip_get_num_entries(archive.get(), 0);
    struct Entry {
        zip_uint64_t index;
        std::string name;
        bool directory;
        std::uint64_t size;
    };
    std::vector<Entry> entries;
    std::uint64_t total = 0;
    for (zip_int64_t i = 0; i < count; ++i) {
        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(archive.get(), static_cast<zip_uint64_t>(i), ZIP_FL_ENC_GUESS, &stat) != 0 ||
            !(stat.valid & ZIP_STAT_NAME)) {
            result.error = "failed to read a zip entry";
            return result;
        }
        std::string name = stat.name;
        if (!is_safe_archive_entry(name)) {
            result.error = "unsafe path in installer payload: " + name;
            return result;
        }
        std::replace(name.begin(), name.end(), '\\', '/');
        const bool directory = !name.empty() && name.back() == '/';
        const std::uint64_t size = (stat.valid & ZIP_STAT_SIZE) ? stat.size : 0;
        total += size;
        if (total > kMaxExtractedBytes) {
            result.error = "installer payload is too large";
            return result;
        }
        entries.push_back({static_cast<zip_uint64_t>(i), name, directory, size});
    }

    std::error_code ec;
    // 包里最深的文件已有 110 多个字符,安装目录稍长就会超过 MAX_PATH:文件 IO 一律走扩展长度
    // 路径(\\?\ 前缀不认正斜杠,条目名先转成本机分隔符),报错仍用原始写法。
    const fs::path root = to_extended_length_path(destination);
    fs::create_directories(root, ec);
    if (ec) {
        result.error = "failed to create " + path_to_utf8(destination) + ": " + ensure_utf8(ec.message());
        return result;
    }
    std::vector<char> buffer(kCopyChunk);
    std::uint64_t done = 0;
    if (observer) observer->on_extract_progress(0, total);
    for (const auto& entry : entries) {
        const fs::path target = root / path_from_utf8(entry.name).make_preferred();
        if (entry.directory) {
            fs::create_directories(target, ec);
            if (ec) {
                result.error = "failed to create " + entry.name + ": " + ensure_utf8(ec.message());
                return result;
            }
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            result.error = "failed to create the folder for " + entry.name + ": " + ensure_utf8(ec.message());
            return result;
        }
        std::unique_ptr<zip_file_t, ZipFileCloser> file(zip_fopen_index(archive.get(), entry.index, 0));
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!file || !out) {
            result.error = "failed to extract " + entry.name;
            return result;
        }
        std::uint64_t written = 0;
        while (true) {
            const zip_int64_t n = zip_fread(file.get(), buffer.data(), buffer.size());
            if (n < 0) {
                result.error = "failed to read " + entry.name + " from the installer payload";
                return result;
            }
            if (n == 0) break;
            out.write(buffer.data(), static_cast<std::streamsize>(n));
            written += static_cast<std::uint64_t>(n);
            done += static_cast<std::uint64_t>(n);
            if (written > entry.size || !out) {
                result.error = "failed to write " + entry.name;
                return result;
            }
            if (observer) observer->on_extract_progress(done, total);
        }
        out.close();
        if (!out || written != entry.size) {
            result.error = "failed to write " + entry.name;
            return result;
        }
        ++result.files;
    }
    result.ok = true;
    result.bytes = total;
    if (observer) observer->on_extract_progress(total, total);
    return result;
}

}  // namespace acecode::installer
