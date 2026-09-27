#include "hook_events.hpp"

#include "hooks/hook_config.hpp"
#include "platform/process/os_process.hpp"
#include "utils/uuid.hpp"

#include <utility>

namespace acecode {
namespace {

std::string registry_source_kind_to_string(RegistrySource::Kind kind) {
    switch (kind) {
    case RegistrySource::Kind::Empty: return "empty";
    case RegistrySource::Kind::Bundled: return "bundled";
    case RegistrySource::Kind::UserOverride: return "user_override";
    case RegistrySource::Kind::Network: return "network";
    }
    return "unknown";
}

nlohmann::json model_profile_to_json(const ModelProfile& profile) {
    nlohmann::json j = {
        {"name", profile.name},
        {"provider", profile.provider},
        {"model", profile.model},
    };
    if (!profile.base_url.empty()) j["base_url"] = profile.base_url;
    if (profile.models_dev_provider_id.has_value()) {
        j["models_dev_provider_id"] = *profile.models_dev_provider_id;
    }
    if (profile.stream_timeout_ms.has_value()) {
        j["stream_timeout_ms"] = *profile.stream_timeout_ms;
    }
    return j;
}

nlohmann::json assistant_tool_calls_to_json(const nlohmann::json& tool_calls) {
    nlohmann::json out = nlohmann::json::array();
    if (!tool_calls.is_array()) return out;
    for (const auto& item : tool_calls) {
        nlohmann::json one;
        if (item.contains("id") && item["id"].is_string()) {
            one["id"] = item["id"].get<std::string>();
        }
        if (item.contains("function") && item["function"].is_object()) {
            const auto& fn = item["function"];
            one["name"] = fn.value("name", std::string{});
            one["arguments"] = fn.value("arguments", std::string{});
        }
        if (!one.empty()) out.push_back(std::move(one));
    }
    return out;
}

} // namespace

nlohmann::json hook_registry_source_to_json(const RegistrySource& source) {
    nlohmann::json j = {
        {"kind", registry_source_kind_to_string(source.kind)},
        {"path_or_url", source.path_or_url},
    };
    if (source.seed_dir.has_value()) j["seed_dir"] = *source.seed_dir;
    if (source.manifest.has_value() && source.manifest->is_object()) {
        const auto& m = *source.manifest;
        nlohmann::json manifest = nlohmann::json::object();
        for (const char* key : {"generated_at", "source_url", "etag", "sha256"}) {
            if (m.contains(key)) manifest[key] = m[key];
        }
        if (!manifest.empty()) j["manifest"] = std::move(manifest);
    }
    return j;
}

nlohmann::json build_startup_models_loaded_payload(
    const std::string& cwd,
    const ModelProfile& active_profile,
    const std::shared_ptr<LlmProvider>& provider) {
    nlohmann::json active = model_profile_to_json(active_profile);
    if (provider) {
        active["provider"] = provider->name();
        active["model"] = provider->model();
    }

    return {
        {"schema_version", 1},
        {"event", kHookEventStartupModelsLoaded},
        {"timestamp", iso_timestamp()},
        {"process", {
            {"pid", static_cast<int>(daemon::current_pid())},
            {"cwd", cwd},
        }},
        {"models", {
            {"registry_source", hook_registry_source_to_json(current_registry_source())},
            {"active", std::move(active)},
        }},
    };
}

nlohmann::json build_assistant_message_completed_payload(
    const std::string& cwd,
    const std::string& session_id,
    const std::string& provider_name,
    const std::string& model,
    const ChatMessage& assistant_message) {
    const bool has_tool_calls =
        assistant_message.tool_calls.is_array() && !assistant_message.tool_calls.empty();

    nlohmann::json assistant = {
        {"kind", has_tool_calls ? "tool_calls" : "text"},
        {"role", assistant_message.role},
        {"content", assistant_message.content},
        {"has_tool_calls", has_tool_calls},
        {"tool_calls", assistant_tool_calls_to_json(assistant_message.tool_calls)},
    };
    if (!assistant_message.reasoning_content.empty()) {
        assistant["reasoning_content"] = assistant_message.reasoning_content;
    }

    return {
        {"schema_version", 1},
        {"event", kHookEventAssistantMessageCompleted},
        {"timestamp", iso_timestamp()},
        {"session", {
            {"id", session_id},
            {"cwd", cwd},
        }},
        {"model", {
            {"provider", provider_name},
            {"model", model},
        }},
        {"assistant", std::move(assistant)},
    };
}

} // namespace acecode
