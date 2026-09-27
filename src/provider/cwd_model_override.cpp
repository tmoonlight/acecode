// cwd_model_override 实现。原子写策略参考 input_history_store.cpp。
#include "cwd_model_override.hpp"

#include "session/session_storage.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace acecode {

std::string cwd_model_override_path(const std::string& cwd_utf8) {
    std::string project_dir = SessionStorage::get_project_dir(cwd_utf8);
    return path_to_utf8(path_from_utf8(project_dir) / "model_override.json");
}

namespace {

std::vector<std::string> legacy_override_paths(const std::string& cwd_utf8,
                                               const std::string& canonical_path) {
    std::vector<std::string> paths;
#ifdef _WIN32
    if (std::none_of(cwd_utf8.begin(), cwd_utf8.end(),
                     [](unsigned char c) { return c >= 0x80; })) {
        return paths;
    }

    std::string native = cwd_utf8;
    std::replace(native.begin(), native.end(), '/', '\\');
    std::string generic = cwd_utf8;
    std::replace(generic.begin(), generic.end(), '\\', '/');
    for (const auto& spelling : {cwd_utf8, native, generic}) {
        try {
            // 仅兼容旧存储键时重现旧版的代码页转换。正斜杠形态可能无法
            // 解码,但同一目录的 TUI 反斜杠形态仍可能存有可读取的设置。
            const auto legacy_cwd = path_to_utf8(fs::path(spelling));
            auto path = cwd_model_override_path(legacy_cwd);
            if (path != canonical_path &&
                std::find(paths.begin(), paths.end(), path) == paths.end()) {
                paths.push_back(std::move(path));
            }
        } catch (const std::exception&) {
            // 旧版不能解析的拼写不会有对应设置,也不能阻塞新会话。
        }
    }
#else
    (void)cwd_utf8;
    (void)canonical_path;
#endif
    return paths;
}

std::optional<std::string> read_override_file(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path_from_utf8(path), ec) || ec) return std::nullopt;

    try {
        std::ifstream ifs(path_from_utf8(path));
        if (!ifs.is_open()) {
            LOG_WARN(std::string("[cwd_model_override] cannot open ") + path);
            return std::nullopt;
        }
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (!j.is_object() || !j.contains("model_name") || !j["model_name"].is_string()) {
            LOG_WARN(std::string("[cwd_model_override] malformed or missing model_name in ") + path);
            return std::nullopt;
        }
        std::string name = j["model_name"].get<std::string>();
        if (name.empty()) return std::nullopt;
        return name;
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[cwd_model_override] parse failure: ") + e.what() + " (" + path + ")");
        return std::nullopt;
    }
}

} // namespace

std::optional<std::string> load_cwd_model_override(const std::string& cwd_utf8) {
    const std::string path = cwd_model_override_path(cwd_utf8);
    std::error_code ec;
    const bool canonical_exists = fs::exists(path_from_utf8(path), ec);
    if (ec) return std::nullopt;
    // 已有新文件即为权威来源,损坏或空值也不能重新启用旧设置。
    if (canonical_exists) return read_override_file(path);
    for (const auto& legacy_path : legacy_override_paths(cwd_utf8, path)) {
        if (auto name = read_override_file(legacy_path)) return name;
    }
    return std::nullopt;
}

void save_cwd_model_override(const std::string& cwd_utf8, const std::string& name) {
    std::string path = cwd_model_override_path(cwd_utf8);
    std::string tmp = path + ".tmp";

    try {
        fs::path p = path_from_utf8(path);
        std::error_code ec;
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path(), ec);
            if (ec) {
                LOG_ERROR(std::string("[cwd_model_override] create_directories failed: ") + ec.message());
                return;
            }
        }

        {
            std::ofstream ofs(path_from_utf8(tmp), std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) {
                LOG_ERROR(std::string("[cwd_model_override] cannot open tmp: ") + tmp);
                return;
            }
            nlohmann::json j;
            j["model_name"] = name;
            ofs << j.dump(2) << '\n';
        }

        ec.clear();
        fs::rename(path_from_utf8(tmp), path_from_utf8(path), ec);
        if (ec) {
            // Windows: rename 到已存在文件会失败,回退 remove + rename。
            fs::remove(path_from_utf8(path), ec);
            ec.clear();
            fs::rename(path_from_utf8(tmp), path_from_utf8(path), ec);
            if (ec) {
                LOG_ERROR(std::string("[cwd_model_override] rename failed: ") + ec.message());
                fs::remove(path_from_utf8(tmp), ec);  // 别遗留 .tmp
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[cwd_model_override] save exception: ") + e.what());
        std::error_code ec;
        fs::remove(path_from_utf8(tmp), ec);
    }
}

void remove_cwd_model_override(const std::string& cwd_utf8) {
    const std::string path = cwd_model_override_path(cwd_utf8);
    auto paths = legacy_override_paths(cwd_utf8, path);
    paths.push_back(path);
    for (const auto& candidate : paths) {
        std::error_code ec;
        fs::remove(path_from_utf8(candidate), ec);
        if (ec) {
            LOG_WARN(std::string("[cwd_model_override] remove failed: ") + ec.message());
        }
    }
}

} // namespace acecode
