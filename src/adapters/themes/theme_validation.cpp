#include "theme_store.hpp"
#include "theme_validation.hpp"
#include "image/image_processor.hpp"
#include "utils/sha256.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <string_view>
#include <utility>
#include <curl/curl.h>

namespace acecode::themes {
using nlohmann::json;

namespace {
const std::set<std::string> kColors = {
    "bg", "surface", "surface-alt", "surface-hi", "shell-hi", "shell-bg",
    "border", "border-soft", "fg", "fg-2", "fg-mute", "accent", "accent-bg",
    "accent-soft", "ok", "ok-bg", "ok-border", "warn", "warn-bg", "danger",
    "danger-bg", "code-bg", "code-fg", "code-line", "selection", "on-selection",
    "send-bg", "send-fg"
};

// The events web/src/lib/themeSounds.js plays (THEME_SOUND_NAMES).
const std::set<std::string> kSoundNames = {
    "click", "select", "toggle", "send", "queue", "stop",
    "complete", "alert", "error", "boot", "switch"
};

const json* declared_sound_files(const json& definition) {
    const auto sounds = definition.find("sounds");
    if (sounds == definition.end() || !sounds->is_object()) return nullptr;
    const auto files = sounds->find("files");
    return files == sounds->end() ? nullptr : &*files;
}

} // namespace

namespace detail {
bool hex(const json& value, std::size_t length, bool prefix) {
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

bool png(const std::string& bytes) {
    return bytes.size() >= 24 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0;
}

// An Ogg page whose first packet is the Opus identification header.
bool ogg_opus(const std::string& bytes) {
    return bytes.size() >= 47 && bytes.compare(0, 4, "OggS") == 0 && bytes.compare(28, 8, "OpusHead") == 0;
}

bool theme_sound_file_name(const std::string& file_name) {
    constexpr std::string_view prefix = "sound-";
    constexpr std::string_view suffix = ".ogg";
    if (file_name.size() <= prefix.size() + suffix.size() || file_name.compare(0, prefix.size(), prefix) != 0 ||
        file_name.compare(file_name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    return kSoundNames.count(file_name.substr(prefix.size(), file_name.size() - prefix.size() - suffix.size())) != 0;
}

void validate_theme_files(const json& definition, const ThemePackageFiles& files, bool decode_legacy_images) {
    const auto images = theme_image_files(definition);
    const auto sounds = theme_sound_files(definition);
    if (files.size() != images.size() + sounds.size() + 1 || !files.count("theme.json"))
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
    for (const auto& [sound, name] : sounds) {
        const auto found = files.find(name);
        if (found == files.end()) throw ThemeError(422, "THEME_INVALID_PACKAGE", "Missing theme sound: " + name);
        const auto& bytes = found->second;
        const auto& expected = declared_sound_files(definition)->at(sound);
        total += bytes.size();
        if (total > kMaxPackageBytes || !ogg_opus(bytes) || bytes.size() != expected.at("bytes") ||
            sha256_hex(bytes) != expected.at("sha256"))
            throw ThemeError(422, "THEME_INVALID_PACKAGE", "Theme sound integrity or size check failed");
    }
}

} // namespace detail

using detail::hex;
using detail::version_ok;
using detail::asset_ok;
using detail::png;
using detail::validate_theme_files;
using detail::kMaxPackageBytes;
using detail::kMaxPreviewBytes;
using detail::kMaxSoundBytes;

std::string theme_workshop_base(const std::string& update_base) {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    const auto invalid = [] { return ThemeError(400, "THEME_WORKSHOP_ADDRESS_INVALID", "Invalid workshop address in update configuration"); };
    if (!url || update_base.find('\0') != std::string::npos ||
        curl_url_set(url.get(), CURLUPART_URL, update_base.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) throw invalid();
    const auto part = [&](CURLUPart key, unsigned flags = 0) {
        char* raw = nullptr;
        const auto status = curl_url_get(url.get(), key, &raw, flags);
        std::unique_ptr<char, decltype(&curl_free)> value(raw, curl_free);
        if (status != CURLUE_OK) throw invalid();
        return std::string(value.get());
    };
    const auto scheme = part(CURLUPART_SCHEME);
    if (scheme != "http" && scheme != "https") throw invalid();
    const auto port = std::stoi(part(CURLUPART_PORT, CURLU_DEFAULT_PORT));
    if (port < 1 || port > 55535) throw invalid();
    if (curl_url_set(url.get(), CURLUPART_PORT, std::to_string(port + 10000).c_str(), 0) != CURLUE_OK ||
        curl_url_set(url.get(), CURLUPART_PATH, "/", 0) != CURLUE_OK ||
        curl_url_set(url.get(), CURLUPART_QUERY, nullptr, 0) != CURLUE_OK ||
        curl_url_set(url.get(), CURLUPART_FRAGMENT, nullptr, 0) != CURLUE_OK) throw invalid();
    return part(CURLUPART_URL);
}

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

std::map<std::string, std::string> theme_sound_files(const json& definition) {
    std::map<std::string, std::string> result;
    if (const auto* files = declared_sound_files(definition); files && files->is_object())
        for (const auto& item : files->items()) result.emplace(item.key(), "sound-" + item.key() + ".ogg");
    return result;
}

std::map<std::string, json> theme_resource_assets(const json& definition) {
    std::map<std::string, json> result;
    for (const auto& [kind, name] : theme_image_files(definition)) result.emplace(name, definition.at(kind));
    for (const auto& [sound, name] : theme_sound_files(definition))
        result.emplace(name, declared_sound_files(definition)->at(sound));
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
        if (d.contains("sounds")) {
            if (!d.at("sounds").is_object()) return false;
            if (const auto* files = declared_sound_files(d)) {
                if (!files->is_object() || files->empty()) return false;
                for (const auto& item : files->items())
                    if (!kSoundNames.count(item.key()) || !asset_ok(item.value(), kMaxSoundBytes)) return false;
            }
        }
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

} // namespace acecode::themes
