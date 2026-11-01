#include "summary_model_fallback.hpp"

#include "config/model_provider_registry.hpp"
#include "platform/unique_sqlite.hpp"
#include "utils/paths.hpp"
#include "utils/sha256.hpp"
#include "utils/utf8_path.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <set>

namespace acecode {
namespace {

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

std::string attempt_identity(ModelProfile profile, const AppConfig& config) {
    profile.name.clear(); // Aliases with identical request settings only run once.
    Sha256 hash;
    hash.update(summary_model_identity(profile, &config));
    const nlohmann::json options = {profile.max_output_tokens.value_or(0),
        profile.reasoning ? model_reasoning_options_to_json(*profile.reasoning) : nlohmann::json(nullptr),
        model_sampling_options_to_json(profile.sampling),
        profile.stream_timeout_ms.value_or(config.openai.stream_timeout_ms)};
    hash.update(options.dump());
    return hash.final_hex();
}

platform::UniqueSqlite open_state(const std::string& path, bool write) {
    platform::UniqueSqlite db;
    std::error_code ec;
    if (write) std::filesystem::create_directories(path_from_utf8(path).parent_path(), ec);
    if (ec || sqlite3_open_v2(path.c_str(), db.put(), write
        ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return {};
    sqlite3_busy_timeout(db.get(), 250);
    if (write && sqlite3_exec(db.get(),
        "CREATE TABLE IF NOT EXISTS model_success (slot TEXT PRIMARY KEY, identity TEXT NOT NULL, at INTEGER NOT NULL)",
        nullptr, nullptr, nullptr) != SQLITE_OK) return {};
    return db;
}

std::string read_identity(const std::string& path, const std::string& slot) {
    auto db = open_state(path, false);
    if (!db) return {};
    sqlite3_stmt* raw = nullptr;
    const auto result = sqlite3_prepare_v2(db.get(), "SELECT identity FROM model_success WHERE slot=?", -1, &raw, nullptr);
    Statement stmt(raw, sqlite3_finalize);
    if (result != SQLITE_OK) return {};
    sqlite3_bind_text(stmt.get(), 1, slot.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) return {};
    return reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
}

void write_identity(const std::string& path, const std::string& slot, const std::string& identity) noexcept {
    try {
        // Capture completion order before waiting for the database writer.
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto db = open_state(path, true);
        if (!db) return;
        sqlite3_stmt* raw = nullptr;
        const auto result = sqlite3_prepare_v2(db.get(),
            "INSERT INTO model_success(slot,identity,at) VALUES(?,?,?) "
            "ON CONFLICT(slot) DO UPDATE SET identity=excluded.identity,at=excluded.at WHERE excluded.at>=model_success.at",
            -1, &raw, nullptr);
        Statement stmt(raw, sqlite3_finalize);
        if (result != SQLITE_OK) return;
        sqlite3_bind_text(stmt.get(), 1, slot.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt.get(), 2, identity.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt.get(), 3, now);
        sqlite3_step(stmt.get());
    } catch (...) { /* Best-effort runtime state must never fail a conversation. */ }
}

} // namespace

std::string summary_model_identity(const ModelProfile& profile, const AppConfig* config) {
    // Runtime reasoning/timeout overrides do not change the connection. A
    // changed endpoint, protocol, credential or header does invalidate success.
    const auto& headers = profile.provider == "openai" && profile.request_headers.empty() && config
        ? config->openai.request_headers : profile.request_headers;
    const nlohmann::json identity = {profile.name, profile.provider, profile.model,
        normalize_model_endpoint_identity(profile.base_url), profile.api_key,
        profile.endpoint_mode.value_or("base_url"), model_profile_api_protocol(profile), headers};
    Sha256 hash;
    hash.update(identity.dump());
    return hash.final_hex();
}

std::string summary_model_state_path() {
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "summary-model-state.sqlite3");
}

void remember_successful_chat_model(const std::string& identity, const std::string& state_path) noexcept {
    if (!identity.empty()) write_identity(state_path, "chat", identity);
}

SummaryModelFallback::SummaryModelFallback(const AppConfig& config, const std::string& preferred_name,
    const std::optional<ModelProfile>& contextual, std::string state_path)
    : state_path_(std::move(state_path)) {
    const auto named = [&](const std::string& name) -> std::optional<ModelProfile> {
        if (!name.empty()) for (const auto& profile : config.saved_models) {
            if (profile.name == name) return profile;
        }
        return std::nullopt;
    };
    const auto preferred = named(preferred_name);
    Sha256 selection;
    selection.update(preferred_name);
    if (preferred) selection.update(attempt_identity(*preferred, config));
    preference_key_ = "summary:" + selection.final_hex();
    std::set<std::string> seen;
    const auto add = [&](std::optional<ModelProfile> profile) {
        if (!profile || profile->model.empty() || !is_runtime_model_provider_enabled(profile->provider)) return;
        if (!seen.insert(attempt_identity(*profile, config)).second) return;
        if (profile->provider == "openai" && profile->request_headers.empty()) {
            profile->request_headers = config.openai.request_headers;
        }
        if (profile->provider == "openai" && !profile->stream_timeout_ms) {
            profile->stream_timeout_ms = config.openai.stream_timeout_ms;
        }
        candidates_.push_back(std::move(*profile));
    };
    const auto add_recorded = [&](const std::string& slot) {
        const auto identity = read_identity(state_path_, slot);
        if (identity.empty()) return;
        for (const auto& profile : config.saved_models) {
            if (summary_model_identity(profile, &config) == identity) { add(profile); return; }
        }
    };
    add_recorded(preference_key_);
    add(preferred);
    add_recorded("chat");
    add(contextual);
    add(named(config.default_model_name));
    for (const auto& profile : config.saved_models) add(profile);
}

std::optional<ModelProfile> SummaryModelFallback::run(
    const std::function<bool(const ModelProfile&)>& attempt, const std::atomic<bool>* cancelled) const {
    for (const auto& profile : candidates_) {
        if (cancelled && cancelled->load()) break;
        bool ok = false;
        try { ok = attempt(profile); } catch (...) { /* Try the next configured connection. */ }
        if (cancelled && cancelled->load()) break;
        if (ok) {
            write_identity(state_path_, preference_key_, summary_model_identity(profile));
            return profile;
        }
    }
    return std::nullopt;
}

} // namespace acecode
