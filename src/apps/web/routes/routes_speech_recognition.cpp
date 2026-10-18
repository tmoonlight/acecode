#include "web/server_impl.hpp"
#include "config/config_mutation.hpp"
#include "speech/speech_client.hpp"

#include <algorithm>

namespace acecode::web {
namespace {
using nlohmann::json;

json availability(const SpeechRecognitionConfig& config, std::uint64_t generation) {
    return {{"enabled", config.enabled}, {"configured", speech_recognition_configured(config)},
            {"max_recording_seconds", config.max_recording_seconds},
            {"timeout_ms", config.timeout_ms}, {"generation", generation}};
}

bool valid_request_id(const std::string& id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}
} // namespace

void WebServer::Impl::register_speech_recognition() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump();
        return with_cors(req, std::move(response));
    };
    CROW_ROUTE(app, "/api/config/speech-recognition").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/config/speech-recognition/test").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/speech-recognition").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/speech-recognition/transcriptions/<string>").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/speech-recognition").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        std::shared_lock<std::shared_mutex> lock(app_config_mu);
        return respond(req, 200, availability(deps.app_config->speech_recognition, speech_config_generation));
    });
    CROW_ROUTE(app, "/api/config/speech-recognition").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        std::shared_lock<std::shared_mutex> lock(app_config_mu);
        auto body = speech_recognition_config_json(deps.app_config->speech_recognition);
        body.update(availability(deps.app_config->speech_recognition, speech_config_generation));
        return respond(req, 200, body);
    });
    CROW_ROUTE(app, "/api/config/speech-recognition").methods(crow::HTTPMethod::PUT)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto patch = json::parse(req.body, nullptr, false);
        if (!patch.is_object()) return respond(req, 400, {{"error", "BAD_JSON"}});
        std::lock_guard<std::shared_mutex> lock(app_config_mu);
        const auto result = mutate_config([&](AppConfig& candidate, std::string& error) {
            return apply_speech_recognition_config(candidate.speech_recognition, patch, error);
        }, deps.config_path, deps.app_config);
        if (!result.ok) return respond(req, result.error_kind == ConfigMutationErrorKind::Validation ? 400 : 500,
            {{"error", result.error_kind == ConfigMutationErrorKind::Validation ? "SPEECH_INVALID_CONFIG" : "PERSIST_FAILED"}});
        deps.app_config->speech_recognition = result.config.speech_recognition;
        ++speech_config_generation;
        {
            std::lock_guard<std::mutex> requests_lock(speech_requests_mu);
            for (const auto& [id, cancelled] : speech_requests) cancelled->store(true);
        }
        auto body = speech_recognition_config_json(deps.app_config->speech_recognition);
        body.update(availability(deps.app_config->speech_recognition, speech_config_generation));
        return respond(req, 200, body);
    });
    CROW_ROUTE(app, "/api/config/speech-recognition/test").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        SpeechRecognitionConfig config;
        {
            std::shared_lock<std::shared_mutex> lock(app_config_mu);
            config = deps.app_config->speech_recognition;
        }
        std::string error;
        if (!apply_speech_recognition_config(config, json::parse(req.body, nullptr, false), error))
            return respond(req, 400, {{"error", "SPEECH_INVALID_CONFIG"}});
        config.timeout_ms = std::min(config.timeout_ms, 30000);
        // Shared only with the owned HTTP operation during this explicit test.
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        try {
            const auto result = speech::transcribe(config, speech::silent_wav(), cancelled, &shutdown_requested);
            return result.ok ? respond(req, 200, {{"ok", true}})
                : respond(req, result.status, {{"error", result.error}});
        } catch (...) { return respond(req, 502, {{"error", "SPEECH_CONNECTION_FAILED"}}); }
    });
    CROW_ROUTE(app, "/api/speech-recognition/transcriptions/<string>").methods(crow::HTTPMethod::Delete)
    ([this, respond](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!valid_request_id(id)) return respond(req, 400, {{"error", "BAD_REQUEST"}});
        std::lock_guard<std::mutex> lock(speech_requests_mu);
        const auto found = speech_requests.find(id);
        if (found != speech_requests.end()) found->second->store(true);
        return respond(req, 200, {{"cancelled", true}});
    });
    CROW_ROUTE(app, "/api/speech-recognition/transcriptions/<string>").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!valid_request_id(id)) return respond(req, 400, {{"error", "BAD_REQUEST"}});
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        SpeechRecognitionConfig config;
        std::uint64_t generation;
        {
            std::shared_lock<std::shared_mutex> lock(app_config_mu);
            config = deps.app_config->speech_recognition;
            generation = speech_config_generation;
        }
        if (!config.enabled || !speech_recognition_configured(config))
            return respond(req, 409, {{"error", "SPEECH_NOT_CONFIGURED"}});
        if (req.body.size() > speech::kMaxAudioBytes) return respond(req, 413, {{"error", "SPEECH_INVALID_AUDIO"}});
        if (!speech::valid_wav(req.body, config.max_recording_seconds))
            return respond(req, 422, {{"error", "SPEECH_INVALID_AUDIO"}});
        // Shared by this handler, cancellation endpoint and owned HTTP operation.
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        {
            std::shared_lock<std::shared_mutex> config_lock(app_config_mu);
            if (generation != speech_config_generation)
                return respond(req, 409, {{"error", "SPEECH_CANCELLED"}});
            std::lock_guard<std::mutex> lock(speech_requests_mu);
            if (shutdown_requested.load()) return respond(req, 503, {{"error", "UNAVAILABLE"}});
            if (!speech_requests.empty()) return respond(req, 429, {{"error", "SPEECH_BUSY"}});
            speech_requests.emplace(id, cancelled);
        }
        speech::Result result;
        try { result = speech::transcribe(config, req.body, cancelled, &shutdown_requested); }
        catch (...) { result.error = "SPEECH_CONNECTION_FAILED"; }
        {
            std::lock_guard<std::mutex> lock(speech_requests_mu);
            speech_requests.erase(id);
        }
        if (cancelled->load()) return respond(req, 409, {{"error", "SPEECH_CANCELLED"}});
        return result.ok ? respond(req, 200, {{"status", result.text.empty() ? "no_speech" : "ok"}, {"text", result.text}})
            : respond(req, result.status, {{"error", result.error}});
    });
}

} // namespace acecode::web
