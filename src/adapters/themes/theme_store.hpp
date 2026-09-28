#pragma once

#include "network/http.hpp"
#include "config/vocab/theme_id.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include "utils/joining_thread.hpp"
#include <nlohmann/json.hpp>

namespace acecode::themes {

struct ThemeRootState;
struct ThemeExportJob;

struct ThemeError : std::runtime_error {
    int status;
    std::string code;
    std::string path;
    ThemeError(int status, std::string code, const std::string& message, std::string path = {});
};

bool valid_theme_definition(const nlohmann::json& definition);
bool valid_theme_colors(const nlohmann::json& colors);
bool valid_theme_appearance(const nlohmann::json& appearance);
bool valid_theme_catalog(const nlohmann::json& catalog);
// Fixed resource names only; optional images must also be declared in the definition.
std::map<std::string, std::string> theme_image_files(const nlohmann::json& definition);

struct ThemeTransport {
    std::function<upgrade::HttpTextResult(const std::string&)> fetch;
    std::function<upgrade::DownloadResult(
        const std::string&, const std::filesystem::path&,
        const upgrade::DownloadProgressCallback&, const upgrade::HttpCancelCheck&)> download;
};

// Owns one cancellable installation; immutable version directories are only
// exposed after validation and an atomic installed-pointer update.
class ThemeStore {
public:
    using UpdateBaseProvider = std::function<std::string()>;
    ThemeStore(std::filesystem::path root, std::string update_base,
               ThemeTransport transport = {});
    ThemeStore(std::filesystem::path root, UpdateBaseProvider update_base,
               ThemeTransport transport = {});
    ~ThemeStore();
    nlohmann::json catalog(bool refresh = false);
    nlohmann::json claim_startup_theme();
    nlohmann::json definition(const std::string& id) const;
    bool installed(const std::string& id) const;
    std::string image(const std::string& id, const std::string& kind);
    nlohmann::json start(const std::string& id, const nlohmann::json& consent);
    nlohmann::json job() const;
    nlohmann::json cancel();
    // Validated local package installation. Only ai-* identifiers are accepted;
    // immutable versions and the installed pointer use the remote store layout.
    nlohmann::json install_local(const nlohmann::json& definition,
                                const std::string& background_png,
                                const std::string& thumbnail_png,
                                const std::map<std::string, std::string>& extra_images = {});
    nlohmann::json preview_import(const std::string& archive_bytes) const;
    nlohmann::json import_archive(const std::string& archive_bytes, const std::string& confirmed_sha256);
    // The picker is trusted native UI, never a destination supplied by HTTP.
    using ExportSavePicker = std::function<std::optional<std::filesystem::path>(const std::string&)>;
    nlohmann::json start_export(const std::string& id, const ExportSavePicker& picker = {});
    nlohmann::json export_job(const std::string& job_id) const;
    nlohmann::json cancel_export(const std::string& job_id);
    std::string export_download(const std::string& job_id) const;
    nlohmann::json remove_local(const std::string& id, const std::function<void()>& commit = {});

private:
    std::filesystem::path root_;
    UpdateBaseProvider update_base_;
    std::string base_;
    ThemeTransport transport_;
    mutable std::mutex mu_;
    std::mutex catalog_mu_;
    std::mutex start_mu_;
    std::mutex preview_mu_;
    nlohmann::json catalog_;
    nlohmann::json job_ = {{"state", "idle"}};
    std::atomic<bool> cancel_{false};
    acecode::JoiningThread worker_;
    std::shared_ptr<ThemeRootState> local_state_;
    mutable std::mutex exports_mu_;
    std::map<std::string, std::shared_ptr<ThemeExportJob>> exports_;
    std::shared_ptr<ThemeExportJob> find_export(const std::string& job_id) const;
    void run_export(const std::shared_ptr<ThemeExportJob>& job);
    nlohmann::json descriptor(const std::string& id);
    std::filesystem::path installed_directory(const std::string& id) const;
    void install(nlohmann::json entry);
    void update_job(const nlohmann::json& patch);
    nlohmann::json local_catalog() const;
};

} // namespace acecode::themes
