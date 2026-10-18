#include "speech_client.hpp"
#include "network/proxy_resolver.hpp"
#include "utils/abandonable_call.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>

namespace acecode::speech {
namespace {
Result failure(const char* code, int status = 502) { return {false, {}, code, status}; }

std::uint32_t read_u32(const std::string& value, std::size_t offset) {
    std::uint32_t out = 0;
    for (unsigned i = 0; i < 4; ++i)
        out |= static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset + i])) << (8 * i);
    return out;
}
void write_u32(std::string& value, std::size_t offset, std::uint32_t number) {
    for (unsigned i = 0; i < 4; ++i) value[offset + i] = static_cast<char>((number >> (8 * i)) & 255);
}

Result send(const SpeechRecognitionConfig& config, const std::string& audio,
            const std::shared_ptr<std::atomic<bool>>& cancelled,
            const network::ProxyOptions& proxy) {
    const auto plan = request_plan(config);
    cpr::Session session;
    session.SetUrl(cpr::Url{plan.url});
    session.SetTimeout(cpr::Timeout{config.timeout_ms});
    session.SetConnectTimeout(cpr::ConnectTimeout{5000});
    session.SetRedirect(cpr::Redirect{false});
    session.SetSslOptions(network::build_ssl_options(proxy));
    session.SetProxies(proxy.proxies);
    session.SetProxyAuth(proxy.auth);
    cpr::Header headers{{"Accept", "application/json"}};
    if (!config.api_key.empty()) headers["Authorization"] = "Bearer " + config.api_key;
    if (config.protocol == "sensevoice") {
        // Compatibility with the original standalone service's Origin check.
        const auto start = config.base_url.find("://") + 3;
        headers["Origin"] = config.base_url.substr(0, config.base_url.find('/', start));
    }
    session.SetHeader(headers);
    cpr::Multipart form{};
    form.parts.emplace_back(plan.file_field,
        cpr::Buffer{audio.begin(), audio.end(), std::filesystem::path("recording.wav")}, "audio/wav");
    for (const auto& [key, value] : plan.fields) form.parts.emplace_back(key, value);
    session.SetMultipart(std::move(form));
    session.SetProgressCallback(cpr::ProgressCallback{[cancelled](cpr::cpr_off_t, cpr::cpr_off_t,
                                                                cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) {
        return !cancelled->load();
    }});
    std::string body;
    session.SetWriteCallback(cpr::WriteCallback{[&body](std::string_view chunk, intptr_t) {
        if (body.size() + chunk.size() > 1024 * 1024) return false;
        body.append(chunk);
        return true;
    }});
    const auto response = session.Post();
    if (cancelled->load()) return failure("SPEECH_CANCELLED", 409);
    if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) return failure("SPEECH_TIMEOUT", 504);
    if (response.error.code != cpr::ErrorCode::OK) return failure("SPEECH_CONNECTION_FAILED");
    return parse_response(static_cast<int>(response.status_code), body);
}
} // namespace

RequestPlan request_plan(const SpeechRecognitionConfig& config) {
    RequestPlan plan;
    auto base = config.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (config.protocol == "sensevoice") return {base + "/api/transcriptions", "audio", {}};
    plan.file_field = "file";
    if (config.protocol == "whisper_cpp") plan.url = base + "/inference";
    else {
        if (base.size() < 3 || base.substr(base.size() - 3) != "/v1") base += "/v1";
        plan.url = base + "/audio/transcriptions";
        plan.fields.emplace_back("model", config.model);
    }
    plan.fields.emplace_back("response_format", "json");
    if (!config.language.empty() && (config.language != "auto" || config.protocol == "whisper_cpp"))
        plan.fields.emplace_back("language", config.language);
    return plan;
}

Result parse_response(int status, const std::string& body) {
    if (status == 401 || status == 403) return failure("SPEECH_AUTH_FAILED");
    if (status == 429) return failure("SPEECH_BUSY", 429);
    if (status == 503) return failure("SPEECH_NOT_READY", 503);
    if (status == 504 || status == 408) return failure("SPEECH_TIMEOUT", 504);
    if (status == 413 || status == 415 || status == 422 || status == 400)
        return failure("SPEECH_INVALID_AUDIO", 422);
    if (status < 200 || status >= 300) return failure("SPEECH_UPSTREAM_FAILED");
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (!parsed.is_object() || parsed.contains("error") || !parsed.contains("text") || !parsed["text"].is_string())
        return failure("SPEECH_INVALID_RESPONSE");
    if (parsed.contains("status") && parsed["status"] != "ok" && parsed["status"] != "no_speech")
        return failure("SPEECH_INVALID_RESPONSE");
    return {true, parsed.value("status", std::string{}) == "no_speech" ? std::string{} : parsed["text"].get<std::string>(), {}, 200};
}

std::string silent_wav() {
    std::string audio(44 + 32000, '\0');
    audio.replace(0, 4, "RIFF"); write_u32(audio, 4, static_cast<std::uint32_t>(audio.size() - 8));
    audio.replace(8, 8, "WAVEfmt "); write_u32(audio, 16, 16);
    audio[20] = 1; audio[22] = 1;
    write_u32(audio, 24, 16000); write_u32(audio, 28, 32000);
    audio[32] = 2; audio[34] = 16;
    audio.replace(36, 4, "data"); write_u32(audio, 40, 32000);
    return audio;
}

bool valid_wav(const std::string& audio, int max_seconds) {
    if (max_seconds <= 0 || audio.size() <= 44 || audio.size() > kMaxAudioBytes || audio.size() % 2 ||
        audio.compare(0, 4, "RIFF") || audio.compare(8, 8, "WAVEfmt ") || audio.compare(36, 4, "data")) return false;
    return read_u32(audio, 4) == audio.size() - 8 && read_u32(audio, 16) == 16 &&
        read_u32(audio, 20) == 0x00010001 && read_u32(audio, 24) == 16000 &&
        read_u32(audio, 28) == 32000 && read_u32(audio, 32) == 0x00100002 &&
        read_u32(audio, 40) == audio.size() - 44 &&
        audio.size() - 44 <= static_cast<std::size_t>(max_seconds) * 32000;
}

Result transcribe(SpeechRecognitionConfig config, std::string audio,
                  std::shared_ptr<std::atomic<bool>> cancelled, const std::atomic<bool>* shutdown) {
    if (!speech_recognition_configured(config)) return failure("SPEECH_NOT_CONFIGURED", 400);
    if (!valid_wav(audio, config.max_recording_seconds)) return failure("SPEECH_INVALID_AUDIO", 422);
    if (cancelled->load()) return failure("SPEECH_CANCELLED", 409);
    auto proxy = config.use_proxy ? network::proxy_options_for(config.base_url) : network::ProxyOptions{};
    if (!config.use_proxy) proxy.proxies = cpr::Proxies{{"http", ""}, {"https", ""}};
    const auto result = run_abandonable<Result>(
        [config = std::move(config), audio = std::move(audio), cancelled, proxy = std::move(proxy)] {
            return send(config, audio, cancelled, proxy);
        }, shutdown ? shutdown : cancelled.get());
    if (result) return *result;
    cancelled->store(true);
    return failure("SPEECH_CANCELLED", 409);
}

} // namespace acecode::speech
