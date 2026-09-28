#include "web/server_impl.hpp"

namespace acecode::web {
using nlohmann::json;

namespace {
std::string attachment_filename(const std::string& filename) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const auto byte : filename) {
        const auto c = static_cast<unsigned char>(byte);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') encoded += static_cast<char>(c);
        else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
    }
    return "attachment; filename=\"theme.zip\"; filename*=UTF-8''" + encoded;
}
}

void WebServer::Impl::register_themes() {
    const auto config_dir = deps.config_path.empty()
        ? path_from_utf8(get_acecode_dir()) : path_from_utf8(deps.config_path).parent_path();
    theme_store = std::make_unique<acecode::themes::ThemeStore>(config_dir / "themes", [this] {
        std::shared_lock<std::shared_mutex> config_lock(app_config_mu);
        return deps.app_config ? deps.app_config->upgrade.base_url : UpgradeConfig{}.base_url;
    });
    const auto respond = [this](const crow::request& req, const std::function<json()>& action) {
        crow::response response;
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        try { response.body = action().dump(); }
        catch (const themes::ThemeError& e) {
            response.code = e.status;
            response.body = json{{"error", e.code}, {"message", e.what()}, {"error_path", e.path}}.dump();
        } catch (const json::exception&) {
            response.code = 400;
            response.body = json{{"error", "BAD_REQUEST"}, {"message", "Invalid theme request"}}.dump();
        } catch (...) {
            response.code = 500;
            response.body = json{{"error", "THEME_OPERATION_FAILED"}, {"message", "Theme operation failed"}}.dump();
        }
        return with_cors(req, std::move(response));
    };

    CROW_ROUTE(app, "/api/themes").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/first-run").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/import/preview").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/import").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/job").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/job/cancel").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/<string>").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/<string>/install").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/<string>/images/<string>").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&, const std::string&) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/<string>/export").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/exports/<string>").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/exports/<string>/cancel").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/themes/exports/<string>/download").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/themes").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        return respond(req, [&] { return theme_store->catalog(req.url_params.get("refresh") != nullptr); });
    });
    CROW_ROUTE(app, "/api/themes/job").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        return respond(req, [&] { return theme_store->job(); });
    });
    CROW_ROUTE(app, "/api/themes/job/cancel").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        return respond(req, [&] { return theme_store->cancel(); });
    });
    CROW_ROUTE(app, "/api/themes/<string>").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        return respond(req, [&] { return theme_store->definition(id); });
    });
    CROW_ROUTE(app, "/api/themes/<string>/install").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (auto rejected = reject_if_migrating(req)) return std::move(*rejected);
        return respond(req, [&] { return theme_store->start(id, json::parse(req.body)); });
    });
    CROW_ROUTE(app, "/api/themes/first-run").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (auto rejected = reject_if_migrating(req)) return std::move(*rejected);
        return respond(req, [&] { return theme_store->claim_startup_theme(); });
    });
    CROW_ROUTE(app, "/api/themes/import/preview").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        return respond(req, [&] {
            if (req.get_header_value("Content-Type") != "application/zip")
                throw themes::ThemeError(415, "THEME_ZIP_REQUIRED", "A complete theme ZIP is required");
            return theme_store->preview_import(req.body);
        });
    });
    CROW_ROUTE(app, "/api/themes/import").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (auto rejected = reject_if_migrating(req)) return std::move(*rejected);
        return respond(req, [&] {
            if (req.get_header_value("Content-Type") != "application/zip")
                throw themes::ThemeError(415, "THEME_ZIP_REQUIRED", "A complete theme ZIP is required");
            if (req.body.size() > 16 * 1024 * 1024)
                throw themes::ThemeError(413, "THEME_PACKAGE_TOO_LARGE", "Theme ZIP exceeds 16 MiB");
            const auto* digest = req.url_params.get("sha256");
            return theme_store->import_archive(req.body, digest ? digest : "");
        });
    });
    CROW_ROUTE(app, "/api/themes/<string>/export").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (auto rejected = reject_if_migrating(req)) return std::move(*rejected);
        return respond(req, [&] {
            const auto body = req.body.empty() ? json::object() : json::parse(req.body);
            if (!body.is_object() || body.size() > (body.contains("native_save") ? 1u : 0u) ||
                (body.contains("native_save") && !body["native_save"].is_boolean()))
                throw themes::ThemeError(400, "BAD_REQUEST", "Only the native_save boolean is supported");
            themes::ThemeStore::ExportSavePicker picker;
            if (body.value("native_save", false)) picker = [this](const std::string& filename) -> std::optional<std::filesystem::path> {
                if (!deps.native_folder_picker_enabled || !deps.native_save_file_picker)
                    throw themes::ThemeError(501, "THEME_NATIVE_SAVE_UNAVAILABLE", "Native theme Save As is unavailable");
                const auto picked = deps.native_save_file_picker(filename);
                if (!picked.error.empty()) throw themes::ThemeError(500, "THEME_SAVE_FAILED", picked.error);
                if (!picked.path || picked.path->empty()) return std::nullopt;
                return path_from_utf8(*picked.path);
            };
            return theme_store->start_export(id, picker);
        });
    });
    CROW_ROUTE(app, "/api/themes/exports/<string>").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req, const std::string& job_id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (auto rejected = reject_if_migrating(req)) return std::move(*rejected);
        return respond(req, [&] { return theme_store->export_job(job_id); });
    });
    CROW_ROUTE(app, "/api/themes/exports/<string>/cancel").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req, const std::string& job_id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (auto rejected = reject_if_migrating(req)) return std::move(*rejected);
        return respond(req, [&] { return theme_store->cancel_export(job_id); });
    });
    CROW_ROUTE(app, "/api/themes/exports/<string>/download").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req, const std::string& job_id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (auto rejected = reject_if_migrating(req)) return std::move(*rejected);
        try {
            crow::response response(200);
            response.body = theme_store->export_download(job_id);
            response.add_header("Content-Type", "application/zip");
            response.add_header("Content-Disposition", attachment_filename(theme_store->export_job(job_id).at("filename")));
            response.add_header("Cache-Control", "private, no-store");
            response.add_header("X-Content-Type-Options", "nosniff");
            return with_cors(req, std::move(response));
        } catch (const themes::ThemeError& error) {
            return respond(req, [&]() -> json { throw error; });
        }
    });
    CROW_ROUTE(app, "/api/themes/<string>").methods(crow::HTTPMethod::DELETE)
    ([this, respond](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (auto rejected = reject_if_migrating(req)) return std::move(*rejected);
        return respond(req, [&] {
            if (!deps.app_config) throw themes::ThemeError(503, "THEME_CONFIG_UNAVAILABLE", "Appearance preferences are unavailable");
            std::lock_guard<std::shared_mutex> config_lock(app_config_mu);
            const auto before = deps.app_config->web_ui;
            const bool active = before.color_theme == id;
            auto result = theme_store->remove_local(id, [&] {
                if (!active) return;
                deps.app_config->web_ui.color_theme = "blue";
                try {
                    if (!deps.config_path.empty()) save_config(*deps.app_config, deps.config_path);
                    else save_config(*deps.app_config);
                } catch (const std::exception& error) {
                    deps.app_config->web_ui = before;
                    throw themes::ThemeError(500, "PERSIST_FAILED", error.what());
                }
            });
            if (active) result["color_theme"] = "blue";
            result["ui_preferences"] = ui_preferences_to_json(deps.app_config->web_ui);
            return result;
        });
    });
    CROW_ROUTE(app, "/api/themes/<string>/images/<string>").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req, const std::string& id, const std::string& kind) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        try {
            crow::response response(200);
            response.body = theme_store->image(id, kind);
            response.add_header("Content-Type", "image/png");
            // The installed version behind this URL can change after an update.
            // The frontend caches decoded resources until an explicit refresh.
            response.add_header("Cache-Control", "private, no-store");
            response.add_header("X-Content-Type-Options", "nosniff");
            return with_cors(req, std::move(response));
        } catch (const themes::ThemeError& e) {
            return respond(req, [&]() -> json { throw e; });
        } catch (...) {
            return respond(req, []() -> json { throw themes::ThemeError(502, "THEME_PREVIEW_FAILED", "Could not load theme image"); });
        }
    });
}
} // namespace acecode::web
