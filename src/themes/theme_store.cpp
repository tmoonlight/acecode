#include "theme_store.hpp"
#include "utils/base64.hpp"
#include "theme_package.hpp"

#include "upgrade/version.hpp"
#include "image/image_processor.hpp"
#include "utils/atomic_file.hpp"
#include "utils/sha256.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <zip.h>

namespace acecode::themes {
namespace fs = std::filesystem;
using nlohmann::json;

struct ThemeRootState {
    std::recursive_mutex mutex;
    std::set<std::string> active_exports;
};

struct ThemeExportJob {
    mutable std::mutex mutex;
    json info;
    std::atomic<bool> cancelled{false};
    std::thread worker;
    fs::path destination;
    fs::path archive;
    std::string package_sha256;
    bool published = false;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
};

namespace {
constexpr std::uintmax_t kMaxPackageBytes = 16 * 1024 * 1024;
constexpr std::uintmax_t kMaxPreviewBytes = 256 * 1024;
constexpr std::size_t kMaxExportJobs = 32;

std::shared_ptr<ThemeRootState> shared_root_state(const fs::path& root) {
    static std::mutex mutex;
    static std::map<std::string, std::weak_ptr<ThemeRootState>> roots;
    auto key = path_to_utf8(fs::weakly_canonical(fs::absolute(root)));
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = roots.begin(); it != roots.end();) {
        if (it->second.expired()) it = roots.erase(it); else ++it;
    }
    auto& slot = roots[key];
    auto state = slot.lock();
    if (!state) { state = std::make_shared<ThemeRootState>(); slot = state; }
    return state;
}

void require_local(const std::string& id) {
    if (id == "blue" || id == "orange" || is_downloadable_theme(id))
        throw ThemeError(403, "THEME_BUILTIN_PROTECTED", "Built-in themes cannot be exported or deleted");
    if (!is_local_theme(id)) throw ThemeError(400, "THEME_INVALID_ID", "Invalid custom theme identifier");
}

void reject_link(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory) return;
    if (ec) throw ThemeError(422, "THEME_UNSAFE_PATH", "Could not verify theme path", path_to_utf8(path));
    bool linked = fs::is_symlink(status);
#ifdef _WIN32
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) linked = true;
#endif
    if (linked) throw ThemeError(422, "THEME_UNSAFE_PATH", "Theme paths cannot contain symbolic links or reparse points", path_to_utf8(path));
}

fs::path checked_path(const fs::path& root, const fs::path& relative) {
    if (relative.is_absolute()) throw ThemeError(422, "THEME_UNSAFE_PATH", "Theme path must be inside the theme directory");
    auto path = root;
    reject_link(path);
    for (const auto& part : relative) {
        if (part == "..") throw ThemeError(422, "THEME_UNSAFE_PATH", "Theme path escaped its storage directory");
        path /= part;
        reject_link(path);
    }
    return path;
}

void check_tree(const fs::path& root, const fs::path& relative) {
    const auto directory = checked_path(root, relative);
    std::error_code ec;
    if (!fs::exists(directory, ec)) return;
    std::size_t count = 0;
    for (fs::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        reject_link(it->path());
        if (++count > 4096) throw ThemeError(422, "THEME_UNSAFE_PATH", "Theme directory contains too many resources");
    }
    if (ec) throw ThemeError(422, "THEME_UNSAFE_PATH", "Could not verify theme directory", path_to_utf8(directory));
}

bool export_busy(const json& info) {
    const auto state = info.value("state", "failed");
    return state == "preparing" || state == "compressing" || state == "saving";
}

std::string export_filename(const json& definition) {
    std::string name = definition.at("name");
    for (char& character : name) {
        const auto c = static_cast<unsigned char>(character);
        if (c < 32 || c == 127 || std::string("<>:\"/\\|?*").find(character) != std::string::npos) character = '_';
    }
    if (name.size() > 144) {
        std::size_t end = 144;
        while (end && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
        name.resize(end);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name.empty()) name = definition.at("id").get<std::string>();
    return "ACECode-" + name + ".zip";
}

void replace_file(const fs::path& source, const fs::path& destination) {
    std::error_code ec;
    fs::rename(source, destination, ec);
#ifdef _WIN32
    if (ec && ::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) ec.clear();
#endif
    if (ec) throw ThemeError(500, "THEME_SAVE_FAILED", "Could not save theme package", path_to_utf8(destination));
}
const std::set<std::string> kColors = {
    "bg", "surface", "surface-alt", "surface-hi", "shell-hi", "shell-bg",
    "border", "border-soft", "fg", "fg-2", "fg-mute", "accent", "accent-bg",
    "accent-soft", "ok", "ok-bg", "ok-border", "warn", "warn-bg", "danger",
    "danger-bg", "code-bg", "code-fg", "code-line", "selection", "on-selection",
    "send-bg", "send-fg"
};

bool hex(const json& value, std::size_t length, bool prefix = false) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == length && (!prefix || text[0] == '#') &&
        std::all_of(text.begin() + (prefix ? 1 : 0), text.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
}

bool version_ok(const json& value) {
    if (!value.is_string()) return false;
    const auto& s = value.get_ref<const std::string&>();
    return !s.empty() && s.size() <= 40 && s.front() >= '0' && s.front() <= '9' &&
        std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || c == '.' || c == '-' || (c >= 'a' && c <= 'z');
        }) && s.find("..") == std::string::npos;
}

bool asset_ok(const json& asset, std::uintmax_t limit) {
    return asset.is_object() && asset.contains("bytes") && asset["bytes"].is_number_unsigned() &&
        asset["bytes"].get<std::uintmax_t>() > 0 && asset["bytes"].get<std::uintmax_t>() <= limit &&
        asset.contains("sha256") && hex(asset["sha256"], 64);
}

std::string read_file(const fs::path& file, std::uintmax_t limit) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || ec || fs::file_size(file, ec) > limit || ec) {
        throw ThemeError(404, "THEME_NOT_INSTALLED", "Theme resource is unavailable", path_to_utf8(file));
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw ThemeError(404, "THEME_NOT_INSTALLED", "Theme resource is unavailable", path_to_utf8(file));
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

json read_json(const fs::path& file) {
    return json::parse(read_file(file, 128 * 1024));
}

void write_file(const fs::path& file, const std::string& bytes) {
    if (!atomic_write_file(path_to_utf8(file), bytes)) {
        throw ThemeError(500, "THEME_SAVE_FAILED", "Could not save theme resources", path_to_utf8(file));
    }
}

bool matches_file(const fs::path& file, const json& expected) {
    try {
        auto bytes = read_file(file, kMaxPackageBytes);
        return bytes.size() == expected.at("bytes").get<std::uintmax_t>() &&
            sha256_hex(bytes) == expected.at("sha256").get<std::string>();
    } catch (...) { return false; }
}

bool png(const std::string& bytes) {
    return bytes.size() >= 24 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0;
}

bool busy(const json& job) {
    const auto state = job.value("state", "idle");
    return state == "downloading" || state == "installing";
}

std::string theme_base(std::string update_base) {
    while (!update_base.empty() && update_base.back() == '/') update_base.pop_back();
    return update_base + "/themes/";
}

void validate_theme_files(const json& definition, const ThemePackageFiles& files, bool decode_legacy_images = true) {
    const auto images = theme_image_files(definition);
    if (files.size() != images.size() + 1 || !files.count("theme.json"))
        throw ThemeError(422, "THEME_INVALID_PACKAGE", "Theme resources must match the definition");
    std::size_t total = files.at("theme.json").size();
    for (const auto& [kind, name] : images) {
        const auto found = files.find(name);
        if (found == files.end()) throw ThemeError(422, "THEME_INVALID_PACKAGE", "Missing theme image: " + name);
        const auto& bytes = found->second;
        total += bytes.size();
        image::ImageNormalizeOptions options;
        options.force_png = true;
        options.max_edge = 0;
        options.final_max_bytes = kMaxPackageBytes;
        if (total > kMaxPackageBytes || !png(bytes) || bytes.size() != definition.at(kind).at("bytes") ||
            sha256_hex(bytes) != definition.at(kind).at("sha256") ||
            ((decode_legacy_images || (kind != "background" && kind != "thumbnail")) &&
             !image::normalize_image_bytes(bytes, "image/png", options).ok))
            throw ThemeError(422, "THEME_INVALID_PACKAGE", "Theme image integrity or size check failed");
    }
}

std::map<std::string, std::string> unpack_archive(zip_t* archive) {
    const auto count = archive ? zip_get_num_entries(archive, 0) : 0;
    if (count < 3 || count > 5) {
        throw ThemeError(422, "THEME_INVALID_PACKAGE", "Theme archive must contain three to five declared resources");
    }
    std::map<std::string, std::string> files;
    std::size_t total = 0;
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(count); ++i) {
        zip_stat_t info{};
        zip_uint8_t os = 0;
        zip_uint32_t attributes = 0;
        if (zip_stat_index(archive, i, 0, &info) || !info.name ||
            zip_file_get_external_attributes(archive, i, 0, &os, &attributes)) {
            throw ThemeError(422, "THEME_INVALID_PACKAGE", "Invalid archive metadata");
        }
        const std::string name = info.name;
        const auto limit = name == "theme.json" ? 32 * 1024 :
            name == "thumbnail.png" ? kMaxPreviewBytes : kMaxPackageBytes;
        const auto type = (attributes >> 16) & 0170000;
        if ((name != "theme.json" && name != "background.png" && name != "thumbnail.png" &&
             name != "session-background.png" && name != "user-message-background.png") ||
            files.count(name) || !info.size || info.size > limit ||
            ((os == ZIP_OPSYS_UNIX || os == ZIP_OPSYS_OS_X) && type && type != 0100000)) {
            throw ThemeError(422, "THEME_INVALID_PACKAGE", "Unsupported theme archive entry");
        }
        total += static_cast<std::size_t>(info.size);
        if (total > kMaxPackageBytes) throw ThemeError(422, "THEME_INVALID_PACKAGE", "Theme resources exceed the package size limit");
        std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(zip_fopen_index(archive, i, 0), zip_fclose);
        std::string bytes(static_cast<std::size_t>(info.size), '\0');
        std::size_t offset = 0;
        while (file && offset < bytes.size()) {
            const auto n = zip_fread(file.get(), bytes.data() + offset, bytes.size() - offset);
            if (n <= 0) break;
            offset += static_cast<std::size_t>(n);
        }
        if (!file || offset != bytes.size()) {
            throw ThemeError(422, "THEME_INVALID_PACKAGE", "Truncated theme archive entry");
        }
        char extra = 0;
        if (zip_fread(file.get(), &extra, 1) != 0)
            throw ThemeError(422, "THEME_INVALID_PACKAGE", "Theme archive entry checksum or length is invalid");
        files.emplace(name, std::move(bytes));
    }
    return files;
}

std::map<std::string, std::string> unpack(const fs::path& zip_path) {
    int error = 0;
    std::unique_ptr<zip_t, decltype(&zip_discard)> archive(
        zip_open(path_to_utf8(zip_path).c_str(), ZIP_RDONLY, &error), zip_discard);
    return unpack_archive(archive.get());
}

std::map<std::string, std::string> unpack_import(const std::string& bytes) {
    if (bytes.empty() || bytes.size() > kMaxPackageBytes)
        throw ThemeError(413, "THEME_PACKAGE_TOO_LARGE", "Theme ZIP must be between 1 byte and 16 MiB");
    auto* source = zip_source_buffer_create(bytes.data(), bytes.size(), 0, nullptr);
    if (!source) throw ThemeError(422, "THEME_INVALID_PACKAGE", "Could not read theme ZIP");
    std::unique_ptr<zip_t, decltype(&zip_discard)> archive(zip_open_from_source(source, ZIP_RDONLY, nullptr), zip_discard);
    if (!archive) { zip_source_free(source); throw ThemeError(422, "THEME_INVALID_PACKAGE", "Invalid theme ZIP"); }
    auto files = unpack_archive(archive.get());
    try {
        const auto definition = json::parse(files.at("theme.json"));
        if (!valid_theme_definition(definition) || !is_local_theme(definition.value("id", "")))
            throw ThemeError(422, "THEME_INVALID_PACKAGE", "Only custom ACECode theme ZIPs can be imported");
        validate_theme_files(definition, files);
    } catch (const json::exception&) {
        throw ThemeError(422, "THEME_INVALID_PACKAGE", "Invalid theme definition");
    }
    return files;
}
} // namespace

ThemeError::ThemeError(int status, std::string code, const std::string& message, std::string path)
    : std::runtime_error(message), status(status), code(std::move(code)), path(std::move(path)) {}

bool valid_theme_colors(const json& colors) {
    try {
        if (!colors.is_object() || colors.size() != kColors.size()) return false;
        for (const auto& key : kColors) if (!hex(colors.at(key), 7, true)) return false;
        return true;
    } catch (...) { return false; }
}

bool valid_theme_appearance(const json& appearance) {
    if (!appearance.is_object()) return false;
    for (const auto& item : appearance.items()) {
        if (item.key() == "logo_color" || item.key() == "home_title_color" ||
            item.key() == "home_background_color" || item.key() == "session_background_color" ||
            item.key() == "user_message_background_color") {
            if (!hex(item.value(), 7, true)) return false;
        } else if (item.key() == "extend_to_titlebar") {
            if (!item.value().is_boolean()) return false;
        } else if (item.key() == "home_composer_opacity" || item.key() == "home_background_opacity" ||
                   item.key() == "session_background_opacity" || item.key() == "user_message_background_opacity") {
            if (!item.value().is_number()) return false;
            const auto opacity = item.value().get<double>();
            if (!(opacity >= 0.0 && opacity <= 1.0)) return false;
        } else return false;
    }
    return true;
}

std::map<std::string, std::string> theme_image_files(const json& definition) {
    std::map<std::string, std::string> result = {{"background", "background.png"}, {"thumbnail", "thumbnail.png"}};
    if (definition.contains("session_background")) result.emplace("session_background", "session-background.png");
    if (definition.contains("user_message_background")) result.emplace("user_message_background", "user-message-background.png");
    return result;
}

bool valid_theme_definition(const json& d) {
    try {
        const auto id = d.at("id").get<std::string>();
        const bool local = is_local_theme(id);
        if (d.at("schema_version") != 1 || (!is_downloadable_theme(id) && !local) ||
            !version_ok(d.at("version")) ||
            (d.at("mode") != "light" && (id == "eva-01" || d.at("mode") != "dark")) ||
            !valid_theme_colors(d.at("colors")) ||
            (d.contains("appearance") && !valid_theme_appearance(d.at("appearance"))) ||
            !asset_ok(d.at("background"), kMaxPackageBytes) ||
            !asset_ok(d.at("thumbnail"), kMaxPreviewBytes)) return false;
        for (const auto* kind : {"session_background", "user_message_background"})
            if (d.contains(kind) && !asset_ok(d.at(kind), kMaxPackageBytes)) return false;
        if (local || id == kNationalDayThemeId) {
            const auto name = d.at("name").get<std::string>();
            if (name.empty() || name.size() > 256 ||
                name.find_first_not_of(" \t\r\n") == std::string::npos ||
                std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) return false;
        }
        return true;
    } catch (...) { return false; }
}

bool valid_theme_catalog(const json& catalog) {
    try {
        if (catalog.at("schema_version") != 1 || !catalog.at("themes").is_array() ||
            catalog.at("themes").empty() || catalog.at("themes").size() > 2) return false;
        std::set<std::string> ids;
        for (const auto& e : catalog.at("themes")) {
            const auto id = e.at("id").get<std::string>();
            if (!is_downloadable_theme(id) || !ids.insert(id).second || !version_ok(e.at("version")) ||
                !asset_ok(e.at("package"), kMaxPackageBytes) ||
                !asset_ok(e.at("thumbnail"), kMaxPreviewBytes) ||
                !e.at("swatches").is_array() || e.at("swatches").size() != 3) return false;
            const auto prefix = id + "/" + e.at("version").get<std::string>() + "/";
            if (e.at("package").at("path") != prefix + "theme.zip" ||
                e.at("thumbnail").at("path") != prefix + "thumbnail.png") return false;
            for (const auto& color : e.at("swatches")) if (!hex(color, 7, true)) return false;
        }
        return true;
    } catch (...) { return false; }
}

ThemeStore::ThemeStore(fs::path root, std::string update_base, ThemeTransport transport)
    : ThemeStore(std::move(root), [update_base = std::move(update_base)] { return update_base; },
                 std::move(transport)) {}

ThemeStore::ThemeStore(fs::path root, UpdateBaseProvider update_base, ThemeTransport transport)
    : root_(fs::absolute(root).lexically_normal()), update_base_(std::move(update_base)),
      base_(theme_base(update_base_())), transport_(std::move(transport)), local_state_(shared_root_state(root_)) {
    if (!transport_.fetch) transport_.fetch = [](const std::string& url) { return upgrade::fetch_text(url, 10000); };
    if (!transport_.download) transport_.download = [](const std::string& url, const fs::path& path,
            const upgrade::DownloadProgressCallback& progress, const upgrade::HttpCancelCheck& cancel) {
        return upgrade::download_to_file(url, path, 120000, progress, cancel);
    };
    try {
        auto cached = read_json(root_ / "catalog-v2.json");
        if (cached.value("source_base_url", "") == base_ && valid_theme_catalog(cached.at("catalog"))) {
            catalog_ = std::move(cached["catalog"]);
        }
    }
    catch (...) {}
}

ThemeStore::~ThemeStore() {
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
    for (auto& [id, job] : exports_) job->cancelled.store(true);
    for (auto& [id, job] : exports_) if (job->worker.joinable()) job->worker.join();
}

json ThemeStore::catalog(bool refresh) {
    std::lock_guard<std::mutex> fetch_lock(catalog_mu_);
    const auto current_base = theme_base(update_base_());
    if (base_ != current_base) {
        base_ = current_base;
        catalog_ = nullptr;
    }
    bool offline = false;
    auto catalog_url = base_ + "catalog-v2.json";
    std::string failure_message = "Could not load theme catalog";
    if (refresh || catalog_.is_null()) {
        try {
            auto response = transport_.fetch(catalog_url);
            if (response.status_code == 404 || response.status_code == 410) {
                catalog_url = base_ + "catalog.json";
                response = transport_.fetch(catalog_url);
            }
            if (response.body.size() > 128 * 1024) {
                throw ThemeError(503, "THEME_CATALOG_UNAVAILABLE", "Theme catalog is too large");
            }
            auto parsed = json::parse(response.body, nullptr, false);
            if (response.status_code != 200 || !response.error.empty())
                throw ThemeError(503, "THEME_CATALOG_UNAVAILABLE", response.error.empty()
                    ? "Theme catalog request failed: HTTP " + std::to_string(response.status_code) : response.error);
            if (!valid_theme_catalog(parsed))
                throw ThemeError(503, "THEME_CATALOG_UNAVAILABLE", "Invalid theme catalog response");
            catalog_ = std::move(parsed);
            write_file(root_ / "catalog-v2.json", json{{"source_base_url", base_}, {"catalog", catalog_}}.dump());
        } catch (const std::exception& error) { offline = true; failure_message = error.what(); }
        catch (...) { offline = true; }
    }
    const auto local_themes = local_catalog();
    if (catalog_.is_null() && local_themes.empty() && !installed("eva-01") && !installed(kNationalDayThemeId))
        throw ThemeError(503, "THEME_CATALOG_UNAVAILABLE", failure_message, catalog_url);
    auto result = catalog_.is_null() ? json{{"schema_version", 1}, {"themes", json::array()}} : catalog_;
    for (const auto* id : {kNationalDayThemeId, "eva-01"}) {
        if (std::any_of(result["themes"].begin(), result["themes"].end(),
                [id](const json& entry) { return entry.at("id") == id; })) continue;
        const bool present = installed(id);
        if (std::string(id) == kNationalDayThemeId && !present) continue;
        if (!catalog_.is_null() && !present) continue;
        const bool national_day = std::string(id) == kNationalDayThemeId;
        json fallback = {{"id", id}, {"name", national_day ? "国庆节" : "EVA 初号机"}, {"source", "remote"},
            {"available", false}, {"installed", present}, {"update_available", false},
            {"swatches", national_day ? json{"#FFF8F2", "#FFFFFF", "#D9272E"} : json{"#7762A8", "#E9ECF6", "#A9D46A"}}};
        if (fallback["installed"] == true) {
            const auto d = definition(id);
            fallback["version"] = fallback["installed_version"] = d["version"];
        }
        result["themes"].push_back(std::move(fallback));
    }
    for (auto& entry : result["themes"]) {
        if (!entry.contains("package")) continue;
        entry["source"] = "remote";
        entry["available"] = true;
        std::string installed_version;
        try { installed_version = definition(entry["id"]).at("version").get<std::string>(); }
        catch (const ThemeError&) {} // Missing or damaged resources remain downloadable.
        const auto local = upgrade::parse_sem_version(installed_version);
        const auto available = upgrade::parse_sem_version(entry["version"].get<std::string>());
        entry["installed"] = !installed_version.empty();
        entry["installed_version"] = installed_version;
        entry["update_available"] = local && available && upgrade::compare_sem_version(*available, *local) > 0;
        entry["package"]["url"] = base_ + entry["package"]["path"].get<std::string>();
        entry["thumbnail"]["url"] = base_ + entry["thumbnail"]["path"].get<std::string>();
    }
    for (const auto& entry : local_themes) result["themes"].push_back(entry);
    result["offline"] = offline;
    if (offline) result["catalog_error"] = {{"error", "THEME_CATALOG_UNAVAILABLE"},
        {"message", failure_message}, {"error_path", catalog_url}};
    result["job"] = job();
    return result;
}

json ThemeStore::descriptor(const std::string& id) {
    if (!is_downloadable_theme(id)) throw ThemeError(404, "THEME_NOT_FOUND", "Unknown theme");
    const auto entries = catalog().at("themes");
    for (const auto& entry : entries) {
        if (entry.at("id") == id && entry.contains("package")) return entry;
    }
    throw ThemeError(503, "THEME_CATALOG_UNAVAILABLE", "Theme catalog is unavailable", base_ + "catalog-v2.json");
}

json ThemeStore::claim_startup_theme() {
    // Atomic directory creation arbitrates across windows, stores and processes.
    // Retain the marker on failure or interruption: this is one attempt, not a retry policy.
    std::error_code ec;
    fs::create_directories(root_, ec);
    if (ec) throw ThemeError(500, "THEME_SAVE_FAILED", "Could not save startup theme attempt");
    const bool claimed = fs::create_directory(root_ / ".national-day-2026-attempted", ec);
    if (ec) throw ThemeError(500, "THEME_SAVE_FAILED", "Could not save startup theme attempt");
    return {{"id", kNationalDayThemeId}, {"claimed", claimed}};
}

fs::path ThemeStore::installed_directory(const std::string& id) const {
    if (!is_downloadable_theme(id) && !is_local_theme(id))
        throw ThemeError(404, "THEME_NOT_FOUND", "Unknown theme");
    const auto pointer = read_json(is_local_theme(id)
        ? checked_path(root_, fs::path(id) / "installed.json") : root_ / id / "installed.json");
    if (!version_ok(pointer.at("version"))) throw ThemeError(404, "THEME_NOT_INSTALLED", "Theme is not installed");
    const auto relative = fs::path(id) / pointer.at("version").get<std::string>();
    return is_local_theme(id) ? checked_path(root_, relative) : root_ / relative;
}

json ThemeStore::definition(const std::string& id) const {
    std::lock_guard<std::recursive_mutex> lock(local_state_->mutex);
    try {
        const auto dir = installed_directory(id);
        if (is_local_theme(id)) checked_path(root_, dir.lexically_relative(root_) / "theme.json");
        const auto d = read_json(dir / "theme.json");
        if (d.at("id") == id && valid_theme_definition(d) &&
            d.at("version") == dir.filename().string()) {
            bool complete = true;
            for (const auto& [kind, name] : theme_image_files(d)) {
                if (is_local_theme(id)) checked_path(root_, dir.lexically_relative(root_) / name);
                if (!matches_file(dir / name, d.at(kind))) complete = false;
            }
            if (complete) return d;
        }
    } catch (const ThemeError& error) {
        if (error.code == "THEME_UNSAFE_PATH") throw;
    } catch (...) {}
    throw ThemeError(404, "THEME_NOT_INSTALLED", "Theme is not installed or is incomplete");
}

bool ThemeStore::installed(const std::string& id) const {
    try { definition(id); return true; } catch (...) { return false; }
}

json ThemeStore::local_catalog() const {
    std::lock_guard<std::recursive_mutex> lock(local_state_->mutex);
    json entries = json::array();
    std::error_code ec;
    fs::directory_iterator it(root_, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto id = path_to_utf8(it->path().filename());
        if (!is_local_theme(id) || !it->is_directory(ec)) continue;
        try {
            const auto d = definition(id);
            auto thumbnail = d.at("thumbnail");
            thumbnail["url"] = "/api/themes/" + id + "/images/thumbnail";
            entries.push_back({{"id", id}, {"name", d.at("name")}, {"mode", d.at("mode")},
                {"version", d.at("version")}, {"installed_version", d.at("version")},
                {"source", "local"}, {"installed", true}, {"available", true},
                {"update_available", false}, {"thumbnail", thumbnail},
                {"swatches", {d.at("colors").at("accent"), d.at("colors").at("bg"),
                    d.at("colors").at("send-bg")}}});
        } catch (...) { /* Incomplete versions are not usable themes. */ }
    }
    std::sort(entries.begin(), entries.end(), [](const json& a, const json& b) {
        return a.at("id").get<std::string>() < b.at("id").get<std::string>();
    });
    return entries;
}

json ThemeStore::preview_import(const std::string& archive_bytes) const {
    const auto files = unpack_import(archive_bytes);
    return {{"theme", json::parse(files.at("theme.json"))},
        {"package_sha256", sha256_hex(archive_bytes)}, {"package_bytes", archive_bytes.size()},
        {"thumbnail_url", "data:image/png;base64," + base64_encode(files.at("thumbnail.png"))}};
}

json ThemeStore::import_archive(const std::string& archive_bytes, const std::string& confirmed_sha256) {
    if (!hex(confirmed_sha256, 64) || sha256_hex(archive_bytes) != confirmed_sha256)
        throw ThemeError(409, "THEME_IMPORT_CHANGED", "Theme ZIP changed; preview it again before importing");
    const auto files = unpack_import(archive_bytes);
    ThemePackageFiles extra;
    for (const auto* name : {"session-background.png", "user-message-background.png"})
        if (files.count(name)) extra.emplace(name, files.at(name));
    return install_local(json::parse(files.at("theme.json")), files.at("background.png"), files.at("thumbnail.png"), extra);
}

json ThemeStore::install_local(const json& d, const std::string& background_png,
                              const std::string& thumbnail_png, const ThemePackageFiles& extra_images) {
    // Different tool instances share this data root. Serialize the final
    // immutable-version/pointer transaction, never the interactive drafting.
    std::lock_guard<std::recursive_mutex> lock(local_state_->mutex);
    if (!valid_theme_definition(d) || !is_local_theme(d.value("id", "")))
        throw ThemeError(422, "THEME_INVALID_PACKAGE", "Invalid local theme definition");
    ThemePackageFiles files = {
        {"theme.json", d.dump(2)}, {"background.png", background_png}, {"thumbnail.png", thumbnail_png}};
    for (const auto& entry : extra_images)
        if (!files.insert(entry).second) throw ThemeError(422, "THEME_INVALID_PACKAGE", "Duplicate theme resource");
    validate_theme_files(d, files);
    const auto id = d.at("id").get<std::string>();
    const auto version = d.at("version").get<std::string>();
    if (local_state_->active_exports.count(id)) throw ThemeError(409, "THEME_BUSY", "Theme export is in progress");
    const auto destination = checked_path(root_, fs::path(id) / version);
    const auto staging = checked_path(root_, fs::path(id) / (version + ".staging"));
    const auto archive = checked_path(root_, fs::path("exports") / id / (version + ".zip"));
    check_tree(root_, id);
    auto temporary_archive = archive; temporary_archive += ".tmp";
    checked_path(root_, temporary_archive.lexically_relative(root_));
    const auto cleanup = [&] {
        std::error_code ec;
        fs::remove_all(staging, ec);
        fs::remove(temporary_archive, ec);
    };
    try {
        fs::create_directories(staging);
        fs::create_directories(archive.parent_path());
        for (const auto& [name, bytes] : files) write_file(staging / name, bytes);
        write_theme_archive(temporary_archive, files);
        const auto package = read_file(temporary_archive, kMaxPackageBytes);
        if (fs::exists(destination)) {
            for (const auto& [name, bytes] : files) {
                if (read_file(destination / name, kMaxPackageBytes) != bytes)
                    throw ThemeError(409, "THEME_VERSION_CONFLICT", "Local theme version already has different resources");
            }
        } else fs::rename(staging, destination);
        write_file(archive, package);
        write_file(root_ / id / "installed.json", json{{"version", version}}.dump());
        cleanup();
        return {{"id", id}, {"version", version}, {"name", d.at("name")},
            {"installed_path", path_to_utf8(destination)}, {"package_path", path_to_utf8(archive)},
            {"apply", true}};
    } catch (...) { cleanup(); throw; }
}

std::string ThemeStore::image(const std::string& id, const std::string& kind) {
    std::unique_lock<std::recursive_mutex> local_lock(local_state_->mutex);
    if (kind != "background" && kind != "thumbnail" && kind != "session-background" && kind != "user-message-background")
        throw ThemeError(404, "THEME_NOT_FOUND", "Unknown theme resource");
    if (installed(id)) {
        const auto images = theme_image_files(definition(id));
        const auto name = kind + ".png";
        if (std::none_of(images.begin(), images.end(), [&](const auto& entry) { return entry.second == name; }))
            throw ThemeError(404, "THEME_NOT_FOUND", "Theme does not declare this image");
        return read_file(installed_directory(id) / name, kMaxPackageBytes);
    }
    local_lock.unlock();
    if (kind != "thumbnail") throw ThemeError(404, "THEME_NOT_INSTALLED", "Theme is not installed");
    const auto e = descriptor(id);
    std::lock_guard<std::mutex> preview_lock(preview_mu_);
    const auto file = root_ / "previews" / (id + "-" + e.at("version").get<std::string>() + ".png");
    if (!matches_file(file, e.at("thumbnail"))) {
        fs::create_directories(file.parent_path());
        auto tmp = file; tmp += ".download";
        auto response = transport_.download(e.at("thumbnail").at("url").get<std::string>(), tmp,
            {}, [&] { std::error_code ec; return fs::exists(tmp, ec) && fs::file_size(tmp, ec) > kMaxPreviewBytes; });
        if (response.status_code != 200 || !response.error.empty() || !matches_file(tmp, e.at("thumbnail"))) {
            std::error_code ec; fs::remove(tmp, ec);
            throw ThemeError(502, "THEME_PREVIEW_FAILED", "Could not load theme preview");
        }
        auto bytes = read_file(tmp, kMaxPreviewBytes);
        std::error_code ec; fs::remove(tmp, ec);
        if (!png(bytes)) throw ThemeError(422, "THEME_INVALID_PACKAGE", "Invalid preview image");
        write_file(file, bytes);
    }
    return read_file(file, kMaxPreviewBytes);
}

std::shared_ptr<ThemeExportJob> ThemeStore::find_export(const std::string& job_id) const {
    std::lock_guard<std::mutex> lock(exports_mu_);
    const auto it = exports_.find(job_id);
    if (it == exports_.end()) throw ThemeError(404, "THEME_EXPORT_NOT_FOUND", "Theme export job was not found or has expired");
    return it->second;
}

json ThemeStore::export_job(const std::string& job_id) const {
    const auto job = find_export(job_id);
    std::lock_guard<std::mutex> lock(job->mutex);
    return job->info;
}

json ThemeStore::cancel_export(const std::string& job_id) {
    const auto job = find_export(job_id);
    std::lock_guard<std::mutex> lock(job->mutex);
    if (export_busy(job->info) && !job->published) job->cancelled.store(true);
    return job->info;
}

json ThemeStore::start_export(const std::string& id, const ExportSavePicker& picker) {
    require_local(id);
    json d;
    {
        std::lock_guard<std::recursive_mutex> lock(local_state_->mutex);
        if (local_state_->active_exports.count(id)) throw ThemeError(409, "THEME_BUSY", "This theme is already being exported");
        d = definition(id);
        checked_path(root_, fs::path("exports") / id);
        local_state_->active_exports.insert(id);
    }
    const auto release = [&] {
        std::lock_guard<std::recursive_mutex> lock(local_state_->mutex);
        local_state_->active_exports.erase(id);
    };
    try {
        const auto filename = export_filename(d);
        fs::path destination;
        if (picker) {
            const auto picked = picker(filename);
            if (!picked || picked->empty()) { release(); return {{"state", "cancelled"}, {"cancelled", true}}; }
            destination = *picked;
            auto extension = path_to_utf8(destination.extension());
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension != ".zip") destination += ".zip";
            std::error_code ec;
            if (!destination.is_absolute() || destination.filename().empty() ||
                !fs::is_directory(destination.parent_path(), ec) || ec || fs::is_directory(destination, ec))
                throw ThemeError(400, "THEME_INVALID_DESTINATION", "Theme package destination is unavailable", path_to_utf8(destination));
        }
        auto job = std::make_shared<ThemeExportJob>();
        const auto job_id = generate_uuid_v7();
        job->destination = std::move(destination);
        job->info = {{"job_id", job_id}, {"id", id}, {"version", d.at("version")}, {"filename", filename},
            {"state", "preparing"}, {"progress", nullptr}, {"reused", false}, {"native_saved", false}};
        {
            std::lock_guard<std::mutex> lock(exports_mu_);
            while (exports_.size() >= kMaxExportJobs) {
                auto oldest = exports_.end();
                for (auto it = exports_.begin(); it != exports_.end(); ++it) {
                    std::lock_guard<std::mutex> job_lock(it->second->mutex);
                    if (!export_busy(it->second->info) &&
                        (oldest == exports_.end() || it->second->created < oldest->second->created)) oldest = it;
                }
                if (oldest == exports_.end()) throw ThemeError(409, "THEME_BUSY", "Too many theme exports are active");
                if (oldest->second->worker.joinable()) oldest->second->worker.join();
                exports_.erase(oldest);
            }
            exports_.emplace(job_id, job);
            // Publication and the joinable handle are one operation. A fast
            // cached export cannot be pruned before its thread is registered.
            try { job->worker = std::thread([this, job] { run_export(job); }); }
            catch (...) { exports_.erase(job_id); throw; }
        }
        std::lock_guard<std::mutex> lock(job->mutex);
        return job->info;
    } catch (...) { release(); throw; }
}

void ThemeStore::run_export(const std::shared_ptr<ThemeExportJob>& job) {
    const auto id = job->info.at("id").get<std::string>();
    const auto version = job->info.at("version").get<std::string>();
    const auto job_id = job->info.at("job_id").get<std::string>();
    fs::path temporary_archive;
    fs::path temporary_destination;
    bool owns_temporary_destination = false;
    json terminal;
    const auto update = [&](const json& values) { std::lock_guard<std::mutex> lock(job->mutex); job->info.update(values); };
    const auto check_cancelled = [&] {
        if (job->cancelled.load()) throw ThemeError(499, "THEME_CANCELLED", "Theme export cancelled");
    };
    try {
        ThemePackageFiles files;
        bool reused = false;
        {
            std::lock_guard<std::recursive_mutex> lock(local_state_->mutex);
            check_cancelled();
            const auto d = definition(id);
            if (d.at("version") != version) throw ThemeError(409, "THEME_CHANGED", "Theme changed before export");
            const auto directory = installed_directory(id);
            std::uintmax_t total = 0;
            std::vector<std::string> names = {"theme.json"};
            for (const auto& entry : theme_image_files(d)) names.push_back(entry.second);
            for (const auto& name : names) {
                const auto limit = name == "theme.json" ? 32 * 1024 : kMaxPackageBytes;
                auto bytes = read_file(checked_path(root_, directory.lexically_relative(root_) / name), limit);
                total += bytes.size();
                files.emplace(name, std::move(bytes));
            }
            if (total > kMaxPackageBytes) throw ThemeError(422, "THEME_INVALID_PACKAGE", "Theme resources exceed the package size limit");
            job->archive = checked_path(root_, fs::path("exports") / id / (version + ".zip"));
            try {
                read_file(job->archive, kMaxPackageBytes);
                reused = unpack(job->archive) == files;
            } catch (...) { /* A missing or stale derived ZIP must be rebuilt from the validated installation. */ }
            if (!reused) {
                const auto legacy = checked_path(root_, fs::path("exports") / (id + "-" + version + ".zip"));
                try {
                    read_file(legacy, kMaxPackageBytes);
                    if (unpack(legacy) == files) { job->archive = legacy; reused = true; }
                } catch (...) { /* Legacy flat packages are reusable only when their complete content matches. */ }
            }
            fs::create_directories(job->archive.parent_path());
            temporary_archive = checked_path(root_, fs::path("exports") / (job_id + ".tmp.zip"));
        }
        update({{"reused", reused}});
        if (!reused) {
            update({{"state", "compressing"}, {"progress", nullptr}});
            write_theme_archive(temporary_archive, files,
                [&](double progress) { update({{"progress", progress}}); }, [&] { return job->cancelled.load(); });
            check_cancelled();
            read_file(temporary_archive, kMaxPackageBytes);
            if (unpack(temporary_archive) != files) throw ThemeError(422, "THEME_INVALID_PACKAGE", "Exported theme package did not match its resources");
            std::lock_guard<std::recursive_mutex> lock(local_state_->mutex);
            checked_path(root_, job->archive.lexically_relative(root_));
            check_cancelled();
            replace_file(temporary_archive, job->archive);
        }
        check_cancelled();
        const auto bytes = read_file(job->archive, kMaxPackageBytes);
        job->package_sha256 = sha256_hex(bytes);
        if (!job->destination.empty()) {
            update({{"state", "saving"}, {"progress", nullptr}});
            temporary_destination = job->destination;
            temporary_destination += ".acecode-" + job_id + ".tmp";
            if (fs::exists(temporary_destination)) throw ThemeError(409, "THEME_SAVE_FAILED", "Theme save temporary file already exists");
            std::ofstream output(temporary_destination, std::ios::binary | std::ios::trunc);
            if (!output) throw ThemeError(500, "THEME_SAVE_FAILED", "Could not create theme package", path_to_utf8(job->destination));
            owns_temporary_destination = true;
            for (std::size_t offset = 0; offset < bytes.size(); offset += 64 * 1024) {
                check_cancelled();
                const auto count = std::min<std::size_t>(64 * 1024, bytes.size() - offset);
                output.write(bytes.data() + offset, static_cast<std::streamsize>(count));
            }
            output.flush();
            if (!output) throw ThemeError(500, "THEME_SAVE_FAILED", "Could not write theme package", path_to_utf8(job->destination));
            output.close();
            // Cancel and publish are serialized: cancellation cannot report
            // success while a destination file is still about to be replaced.
            std::lock_guard<std::mutex> lock(job->mutex);
            check_cancelled();
            replace_file(temporary_destination, job->destination);
            job->published = true;
            terminal = {{"state", "completed"}, {"progress", nullptr}, {"native_saved", true}};
        } else {
            check_cancelled();
            terminal = {{"state", "completed"}, {"progress", nullptr},
                {"download_url", "/api/themes/exports/" + job_id + "/download"}};
        }
    } catch (const ThemeError& error) {
        terminal = {{"state", error.code == "THEME_CANCELLED" ? "cancelled" : "failed"}, {"progress", nullptr},
            {"error", error.code}, {"message", error.what()}, {"error_path", error.path}};
    } catch (const std::exception& error) {
        terminal = {{"state", "failed"}, {"progress", nullptr}, {"error", "THEME_EXPORT_FAILED"},
            {"message", error.what()}, {"error_path", path_to_utf8(job->archive)}};
    }
    std::error_code ec;
    if (!temporary_archive.empty()) fs::remove(temporary_archive, ec);
    if (owns_temporary_destination) fs::remove(temporary_destination, ec);
    std::lock_guard<std::recursive_mutex> root_lock(local_state_->mutex);
    std::lock_guard<std::mutex> lock(job->mutex);
    local_state_->active_exports.erase(id);
    if (terminal.at("state") == "completed" && !job->published && job->cancelled.load())
        terminal = {{"state", "cancelled"}, {"progress", nullptr}, {"error", "THEME_CANCELLED"},
            {"message", "Theme export cancelled"}, {"error_path", ""}};
    job->info.update(terminal);
}

std::string ThemeStore::export_download(const std::string& job_id) const {
    const auto job = find_export(job_id);
    std::lock_guard<std::recursive_mutex> root_lock(local_state_->mutex);
    std::lock_guard<std::mutex> lock(job->mutex);
    if (job->info.at("state") != "completed") throw ThemeError(409, "THEME_EXPORT_NOT_READY", "Theme export is not available for download");
    const auto id = job->info.at("id").get<std::string>();
    const auto d = definition(id);
    if (d.at("version") != job->info.at("version")) throw ThemeError(409, "THEME_CHANGED", "The installed theme has changed; export it again");
    const auto bytes = read_file(checked_path(root_, job->archive.lexically_relative(root_)), kMaxPackageBytes);
    if (sha256_hex(bytes) != job->package_sha256) throw ThemeError(409, "THEME_CHANGED", "The theme package has changed; export it again");
    return bytes;
}

json ThemeStore::remove_local(const std::string& id, const std::function<void()>& commit) {
    require_local(id);
    std::lock_guard<std::recursive_mutex> lock(local_state_->mutex);
    if (local_state_->active_exports.count(id)) throw ThemeError(409, "THEME_BUSY", "Theme export is in progress");
    definition(id);
    check_tree(root_, id);
    const auto source = checked_path(root_, id);
    const auto package_directory = checked_path(root_, fs::path("exports") / id);
    check_tree(root_, fs::path("exports") / id);
    std::vector<fs::path> packages;
    // Derive exact filenames from this theme's version directories. A prefix
    // match would also delete packages belonging to IDs such as ai-name-2.
    for (const auto& entry : fs::directory_iterator(source)) {
        const auto version = path_to_utf8(entry.path().filename());
        if (!entry.is_directory() || !version_ok(version)) continue;
        const auto package = checked_path(root_, fs::path("exports") / (id + "-" + version + ".zip"));
        if (fs::exists(package)) {
            if (!fs::is_regular_file(package)) throw ThemeError(422, "THEME_UNSAFE_PATH", "Theme package is not a regular file", path_to_utf8(package));
            // Legacy flat names can be ambiguous across IDs and versions.
            // Delete only archives whose own manifest identifies this theme.
            try {
                const auto manifest = json::parse(unpack(package).at("theme.json"));
                if (manifest.at("id") == id && manifest.at("version") == version) packages.push_back(package);
            } catch (...) { /* An unidentifiable legacy archive is not safe to claim. */ }
        }
    }
    const auto quarantine = checked_path(root_, ".deleted-" + generate_uuid_v7());
    std::vector<std::pair<fs::path, fs::path>> moved;
    try {
        fs::create_directory(quarantine);
        fs::rename(source, quarantine / "theme");
        moved.emplace_back(source, quarantine / "theme");
        if (fs::exists(package_directory)) {
            fs::rename(package_directory, quarantine / "packages");
            moved.emplace_back(package_directory, quarantine / "packages");
        }
        for (std::size_t index = 0; index < packages.size(); ++index) {
            const auto destination = quarantine / (std::to_string(index) + ".zip");
            fs::rename(packages[index], destination);
            moved.emplace_back(packages[index], destination);
        }
        check_tree(root_, quarantine.lexically_relative(root_));
        if (commit) commit();
    } catch (...) {
        const auto failure = std::current_exception();
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code ec;
            fs::rename(it->second, it->first, ec);
            if (ec) throw ThemeError(500, "THEME_DELETE_ROLLBACK_FAILED", "Could not restore isolated theme files", path_to_utf8(quarantine));
        }
        std::error_code ec; fs::remove(quarantine, ec);
        std::rethrow_exception(failure);
    }
    std::error_code ec;
    fs::remove_all(quarantine, ec);
    json result = {{"id", id}, {"deleted", true}};
    if (ec) {
        result["cleanup_pending"] = true;
        result["cleanup_message"] = "Theme was removed; isolated files could not yet be cleaned up";
    }
    return result;
}

json ThemeStore::job() const { std::lock_guard<std::mutex> lock(mu_); return job_; }
void ThemeStore::update_job(const json& patch) { std::lock_guard<std::mutex> lock(mu_); job_.update(patch); }

json ThemeStore::start(const std::string& id, const json& consent) {
    std::lock_guard<std::mutex> start_lock(start_mu_);
    if (busy(job())) throw ThemeError(409, "THEME_DOWNLOAD_BUSY", "A theme is already downloading");
    const auto entry = descriptor(id);
    if (!consent.is_object() || consent.value("confirm_download", false) != true ||
        (consent.contains("automatic") && !consent["automatic"].is_boolean()) ||
        !consent.contains("sha256") || consent["sha256"] != entry["package"]["sha256"] ||
        !consent.contains("bytes") || consent["bytes"] != entry["package"]["bytes"] ||
        !consent.contains("version") || consent["version"] != entry["version"]) {
        throw ThemeError(409, "THEME_CONFIRMATION_REQUIRED", "Confirm the current theme package size before downloading");
    }
    if (worker_.joinable()) worker_.join();
    cancel_.store(false);
    {
        std::lock_guard<std::mutex> lock(mu_);
        job_ = {{"id", id}, {"version", entry["version"]}, {"state", "downloading"},
            {"bytes_downloaded", 0}, {"bytes_total", entry["package"]["bytes"]},
            {"automatic", consent.value("automatic", false)}};
    }
    worker_ = std::thread([this, entry] { install(entry); });
    return job();
}

json ThemeStore::cancel() { cancel_.store(true); return job(); }

void ThemeStore::install(json entry) {
    const auto id = entry.at("id").get<std::string>();
    const auto version = entry.at("version").get<std::string>();
    const auto staging = root_ / id / (version + ".staging");
    const auto archive = root_ / id / (version + ".download");
    const auto download_url = entry.at("package").at("url").get<std::string>();
    auto failure_path = download_url;
    const auto expected = entry.at("package").at("bytes").get<std::uintmax_t>();
    auto cleanup = [&] { std::error_code ec; fs::remove(archive, ec); fs::remove_all(staging, ec); };
    try {
        failure_path = path_to_utf8(archive);
        fs::create_directories(archive.parent_path());
        cleanup();
        failure_path = download_url;
        auto response = transport_.download(download_url, archive,
            [this](const upgrade::DownloadProgress& p) { update_job({{"bytes_downloaded", p.bytes_written}}); },
            [this, expected] { return cancel_.load() || job().value("bytes_downloaded", std::uintmax_t{0}) > expected; });
        if (cancel_.load()) throw ThemeError(499, "THEME_CANCELLED", "Theme download cancelled");
        if (response.status_code != 200 || !response.error.empty()) throw ThemeError(502, "THEME_DOWNLOAD_FAILED", "Could not download theme");
        if (!matches_file(archive, entry.at("package"))) throw ThemeError(422, "THEME_INVALID_PACKAGE", "Theme package integrity check failed");
        update_job({{"state", "installing"}});
        const auto files = unpack(archive);
        const auto definition = json::parse(files.at("theme.json"));
        if (!valid_theme_definition(definition) || definition.at("id") != id || definition.at("version") != version) {
            throw ThemeError(422, "THEME_INVALID_PACKAGE", "Invalid theme definition");
        }
        // Keep the existing built-in download checks for its original images;
        // local imports and newly supported optional backgrounds are decoded.
        validate_theme_files(definition, files, false);
        failure_path = path_to_utf8(staging);
        for (const auto& [name, bytes] : files) write_file(staging / name, bytes);
        if (cancel_.load()) throw ThemeError(499, "THEME_CANCELLED", "Theme download cancelled");
        const auto destination = root_ / id / version;
        if (fs::exists(destination)) {
            bool identical = true;
            for (const auto& [name, bytes] : files) {
                try { if (read_file(destination / name, kMaxPackageBytes) != bytes) identical = false; }
                catch (...) { identical = false; }
            }
            if (!identical) {
                bool complete = false;
                try {
                    const auto old = read_json(destination / "theme.json");
                    complete = valid_theme_definition(old) && old.at("id") == id && old.at("version") == version;
                    if (complete) for (const auto& [kind, name] : theme_image_files(old))
                        if (!matches_file(destination / name, old.at(kind))) complete = false;
                } catch (...) {}
                if (complete) throw ThemeError(409, "THEME_VERSION_CONFLICT", "Theme version already exists with different content");
                // Repair only an incomplete installation using the declared, validated
                // resources. Atomic file writes make an interrupted repair retryable.
                for (const auto& [name, bytes] : files) write_file(destination / name, bytes);
            }
        } else fs::rename(staging, destination);
        write_file(root_ / id / "installed.json", json{{"version", version}}.dump());
        update_job({{"state", "completed"}, {"bytes_downloaded", expected}});
    } catch (const ThemeError& error) {
        update_job({{"state", error.code == "THEME_CANCELLED" ? "cancelled" : "failed"}, {"error", error.code},
            {"error_path", error.path.empty() ? failure_path : error.path}});
    } catch (...) { update_job({{"state", "failed"}, {"error", "THEME_INSTALL_FAILED"}, {"error_path", failure_path}}); }
    cleanup();
}
} // namespace acecode::themes
