#include "model_connection_test_handler.hpp"

#include "models_handler.hpp"
#include "provider/provider_factory.hpp"

#include <algorithm>
#include <cctype>
#include <exception>

namespace acecode::web {
namespace {

ModelConnectionTestResult failure(int status, const char* code) {
    return {status, {{"error", code}}};
}

} // namespace

ModelConnectionTestResult test_model_connection(
    const nlohmann::json& body, AppConfig snapshot) {
    if (!body.is_object() ||
        (body.contains("original_name") && !body["original_name"].is_string())) {
        return failure(400, "BAD_REQUEST");
    }

    try {
        // Display names and name conflicts are unrelated to connectivity.
        auto input = body;
        std::string temporary_name = "__model_connection_test__";
        while (find_model_by_name(snapshot, temporary_name)) temporary_name += '_';
        input["name"] = temporary_name;
        std::string parse_error;
        auto draft = parse_model_draft(input, parse_error);
        if (!draft) return failure(400, "BAD_REQUEST");
        if (draft->provider != "openai" && draft->provider != "anthropic") {
            return failure(400, "UNSUPPORTED_MODEL_OPTION");
        }

        const auto original_name = body.value("original_name", std::string{});
        const auto validation = original_name.empty()
            ? add_saved_model(snapshot, *draft)
            : update_saved_model(snapshot, original_name, *draft);
        if (validation != SavedModelEditError::OK) {
            return failure(400, to_string(validation));
        }
        auto profile = find_model_by_name(snapshot, temporary_name);
        if (!profile) return failure(500, "MODEL_TEST_FAILED");
        const int configured_timeout = profile->stream_timeout_ms.value_or(
            snapshot.openai.stream_timeout_ms);
        profile->stream_timeout_ms = configured_timeout > 0
            ? (std::min)(configured_timeout, 30000) : 30000;
        auto provider = create_provider_from_entry(*profile, &snapshot);
        if (!provider) return failure(400, "UNSUPPORTED_MODEL_OPTION");

        ChatMessage message;
        message.role = "user";
        message.content = "Reply with OK.";
        // chat() performs one bounded request; the streaming path retries.
        const auto response = provider->chat({message}, {});
        if (response.provider_error.has_error() || response.finish_reason == "error") {
            const auto kind = response.provider_error.kind;
            auto result = failure(kind == ProviderErrorKind::Timeout ? 504 : 502,
                kind == ProviderErrorKind::Timeout ? "MODEL_TEST_TIMEOUT" :
                kind == ProviderErrorKind::Network ? "MODEL_TEST_NETWORK" :
                kind == ProviderErrorKind::Http ? "MODEL_TEST_HTTP_ERROR" :
                "MODEL_TEST_FAILED");
            if (response.provider_error.status_code > 0) {
                result.body["upstream_status"] = response.provider_error.status_code;
            }
            return result;
        }
        if (std::none_of(response.content.begin(), response.content.end(),
                         [](unsigned char ch) { return !std::isspace(ch); })) {
            return failure(502, "MODEL_TEST_EMPTY_REPLY");
        }
        return {200, {{"ok", true}}};
    } catch (const nlohmann::json::exception&) {
        return failure(400, "BAD_REQUEST");
    } catch (const std::exception&) {
        // Providers and malformed input may include secrets in exception text.
        return failure(502, "MODEL_TEST_FAILED");
    }
}

} // namespace acecode::web
