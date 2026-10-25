#include "model_config_refresh.hpp"

#include "config/vocab/permission_mode.hpp"
#include "image/image_processor.hpp"
#include "saved_models_revision.hpp"
#include "utils/logger.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace acecode {
namespace {

std::string resolve_refresh_config_path(const std::string& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "config.json");
}

// 读 config.json 原始字节;文件不存在时 missing=true 并返回 true(没有可刷新的)。
bool read_refresh_config_bytes(const std::string& config_path,
                               std::string& bytes,
                               bool& missing,
                               std::string* error) {
    missing = false;
    const fs::path native_path = path_from_utf8(config_path);
    std::error_code ec;
    if (!fs::exists(native_path, ec) || ec) {
        missing = true;
        return true;
    }
    std::ifstream ifs(native_path, std::ios::binary);
    if (!ifs.is_open()) {
        if (error) *error = "failed to open config file: " + config_path;
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    return true;
}

// 每份 config.json 最近一次刷新时读到的内容。发消息、刷新模型列表都会检查
// 文件,内容没变就不再解析;调用方持配置写锁,这把锁只护这张表。
std::mutex g_refreshed_config_bytes_mu;
std::map<std::string, std::string> g_refreshed_config_bytes;

void remember_refreshed_config_bytes(const std::string& config_path, const std::string& bytes) {
    std::lock_guard<std::mutex> lock(g_refreshed_config_bytes_mu);
    g_refreshed_config_bytes[config_path] = bytes;
}

bool refreshed_config_bytes_unchanged(const std::string& config_path, const std::string& bytes) {
    std::lock_guard<std::mutex> lock(g_refreshed_config_bytes_mu);
    const auto found = g_refreshed_config_bytes.find(config_path);
    return found != g_refreshed_config_bytes.end() && found->second == bytes;
}

bool has_profile_named(const std::vector<ModelProfile>& profiles, const std::string& name) {
    return std::any_of(profiles.begin(), profiles.end(),
                       [&name](const ModelProfile& profile) { return profile.name == name; });
}

bool apply_default_session_preferences_bytes(AppConfig& cfg,
                                             const std::string& bytes,
                                             std::string* error) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(bytes);
    } catch (const std::exception& e) {
        if (error) *error = std::string("failed to parse config file: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        if (error) *error = "config root must be a JSON object";
        return false;
    }

    // 「压缩图片」:Desktop 每个工作区一个 daemon,设置页只改了当前那个;其它
    // daemon(以及手改 config.json)在下一次发消息前从这里跟上。
    ImageCompressionConfig image_compression;
    if (j.contains("image_compression") && j["image_compression"].is_object()) {
        const auto& icj = j["image_compression"];
        if (icj.contains("enabled") && icj["enabled"].is_boolean()) {
            image_compression.enabled = icj["enabled"].get<bool>();
        }
    }
    cfg.image_compression = image_compression;
    image::set_model_image_compression_enabled(image_compression.enabled);

    AppConfig next = cfg;
    if (j.contains("saved_models")) {
        std::string err;
        auto parsed = parse_saved_models(j["saved_models"], err);
        if (!parsed.has_value()) {
            if (error) *error = err;
            return false;
        }
        next.saved_models = std::move(*parsed);
        // save_config 不写空的 default_model_name,缺省即为空,与启动加载一致。
        next.default_model_name.clear();
    }
    if (j.contains("default_model_name") && j["default_model_name"].is_string()) {
        next.default_model_name = j["default_model_name"].get<std::string>();
    }
    if (j.contains("default_permission_mode") &&
        j["default_permission_mode"].is_string()) {
        next.default_permission_mode = normalize_permission_mode_name(
            j["default_permission_mode"].get<std::string>());
    } else if (!j.contains("default_permission_mode")) {
        next.default_permission_mode = "default";
    }

    if (!next.saved_models.empty()) {
        // 手改时删了默认模型却没改 default_model_name:与启动加载一样退到第一个,
        // 只改内存不回写(下次正常加载或设置页保存时落盘)。
        if (!next.default_model_name.empty() &&
            !has_profile_named(next.saved_models, next.default_model_name)) {
            LOG_WARN("[config] default model '" + next.default_model_name +
                     "' not found after config refresh; using '" +
                     next.saved_models.front().name + "'");
            next.default_model_name = next.saved_models.front().name;
        }
        std::string err;
        if (!validate_saved_models(next.saved_models, next.default_model_name, err)) {
            if (error) *error = err;
            return false;
        }
        sanitize_disabled_model_providers(next);
    } else if (!next.default_model_name.empty()) {
        LOG_WARN("[config] default_model_name ignored because saved_models is empty: " +
                 next.default_model_name);
        next.default_model_name.clear();
    }

    // 模型列表结构变化时推进 revision,已打开的会话在下一次请求前重建 Provider;
    // 只改内存不推进,会话会一直拿旧 key / 旧地址调用。
    publish_live_saved_models(cfg, std::move(next.saved_models));
    cfg.default_model_name = std::move(next.default_model_name);
    cfg.default_permission_mode = std::move(next.default_permission_mode);
    return true;
}

} // namespace

std::string normalize_permission_mode_name(std::string value) {
    // 别名(accept-edits / acceptEdits → auto)集中在 config/vocab/permission_mode.hpp 维护;
    // 老配置里的 accept-edits 读进来即归一成 auto,下次保存写 auto。
    if (auto parsed = parse_permission_mode_name(value)) {
        return permission_mode_name(*parsed);
    }
    if (!value.empty()) {
        LOG_WARN("[config] default_permission_mode='" + value +
                 "' invalid; falling back to 'default'");
    }
    return "default";
}

bool refresh_default_session_preferences_from_config(
    AppConfig& cfg,
    const std::string& explicit_path,
    std::string* error) {
    if (error) error->clear();
    const std::string config_path = resolve_refresh_config_path(explicit_path);
    std::string bytes;
    bool missing = false;
    if (!read_refresh_config_bytes(config_path, bytes, missing, error)) return false;
    if (missing) return true;
    remember_refreshed_config_bytes(config_path, bytes);
    return apply_default_session_preferences_bytes(cfg, bytes, error);
}

bool refresh_model_config_if_changed(AppConfig& cfg, const std::string& explicit_path) {
    const std::string config_path = resolve_refresh_config_path(explicit_path);
    std::string bytes;
    bool missing = false;
    std::string error;
    if (!read_refresh_config_bytes(config_path, bytes, missing, &error)) {
        LOG_WARN("[config] model config refresh skipped: " + error);
        return false;
    }
    if (missing || refreshed_config_bytes_unchanged(config_path, bytes)) return true;
    // 解析失败也记下这份内容:写到一半的文件写完后内容会再变,届时重试;
    // 内容不变就不再每次请求都报同一个错。
    remember_refreshed_config_bytes(config_path, bytes);
    if (!apply_default_session_preferences_bytes(cfg, bytes, &error)) {
        LOG_WARN("[config] config.json changed but was not applied; keeping current model "
                 "profiles: " + error);
        return false;
    }
    return true;
}

} // namespace acecode
