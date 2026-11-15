#include "share_projection.hpp"
#include "memory/secret_redaction.hpp"
#include <algorithm>

namespace acecode::share {
using nlohmann::json;

namespace {
json redact_json_at_depth(json value, int depth) {
    if (depth > 24) return nullptr;
    if (value.is_string()) return redact_secrets(value.get_ref<const std::string&>()).text;
    if (value.is_array() || value.is_object()) {
        for (auto& item : value) item = redact_json_at_depth(std::move(item), depth + 1);
    }
    return value;
}
} // namespace

json redact_share_json(json value) { return redact_json_at_depth(std::move(value), 0); }

json project_message(const json& message, bool include_tools) {
    if (!message.is_object()) return nullptr;
    if (message.value("is_meta", false)) return nullptr;
    const auto role = message.value("role", std::string{});
    if (role != "user" && role != "assistant" && role != "tool") return nullptr;
    const auto meta = message.value("metadata", json::object());
    if (meta.is_object() && (meta.value("hidden_goal_context", false) ||
                            meta.value("hidden", false))) return nullptr;
    json out = json::object();
    for (const auto* key : {"id", "role", "uuid", "timestamp", "tool_call_id", "subtype"})
        if (message.contains(key)) out[key] = message[key];
    if (role != "tool" || include_tools) {
        if (message.contains("content")) out["content"] = message["content"];
        if (role == "user" && meta.is_object() && meta.contains("display_text") && meta["display_text"].is_string())
            out["content"] = meta["display_text"];
        if (include_tools && message.contains("reasoning_content"))
            out["reasoning_content"] = message["reasoning_content"];
    }
    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        out["tool_calls"] = json::array();
        for (const auto& call : message["tool_calls"]) {
            if (!call.is_object()) continue;
            json projected;
            for (const auto* key : {"id", "type", "name"}) if (call.contains(key)) projected[key] = call[key];
            if (call.contains("function") && call["function"].is_object()) {
                projected["function"]["name"] = call["function"].value("name", "");
                if (include_tools) projected["function"]["arguments"] = call["function"].value("arguments", "");
            }
            out["tool_calls"].push_back(std::move(projected));
        }
    }
    if (meta.is_object()) {
        json clean = json::object();
        for (const auto* key : {"display_text", "tool_name", "tool_summary", "status", "duration_ms"})
            if (meta.contains(key)) clean[key] = meta[key];
        if (include_tools) for (const auto* key : {"tool_input", "tool_output", "hunks", "exit_code"})
            if (meta.contains(key)) clean[key] = meta[key];
        if (!clean.empty()) out["metadata"] = std::move(clean);
    }
    // Only attachment IDs are forwarded; the authenticated blob route decides
    // membership and MIME. Never forward data: or file: URLs from remote input.
    if (message.contains("content_parts") && message["content_parts"].is_array()) {
        out["content_parts"] = json::array();
        for (const auto& part : message["content_parts"]) {
            if (!part.is_object()) continue;
            if (part.value("type", "") == "text" && (role != "tool" || include_tools))
                out["content_parts"].push_back({{"type", "text"}, {"text", part.value("text", "")}});
            if (part.value("type", "") == "image" && part.contains("attachment") && part["attachment"].is_object()) {
                const auto& attachment = part["attachment"];
                if (attachment.contains("id") && attachment["id"].is_string())
                    out["content_parts"].push_back({{"type", "image"}, {"attachment", {
                        {"id", attachment["id"]}, {"name", attachment.value("name", "")}}}});
            }
        }
    }
    return redact_share_json(std::move(out));
}

bool guest_sensitive_path(std::string_view path) {
    std::string value(path);
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
    });
    std::size_t start = 0;
    while (start < value.size()) {
        auto end = value.find('/', start);
        if (end == std::string::npos) end = value.size();
        const auto name = value.substr(start, end - start);
        if (name.rfind(".env", 0) == 0 || name.rfind("id_rsa", 0) == 0 ||
            name.rfind("id_ed25519", 0) == 0 || name.rfind("id_ecdsa", 0) == 0 ||
            name == ".npmrc" || name == ".netrc" || name == ".git-credentials" ||
            name == ".ssh" || name == ".aws" || name == ".gnupg") return true;
        for (const auto* suffix : {".pem", ".key", ".p12", ".pfx", ".crt", ".cer"}) {
            const std::string extension(suffix);
            if (name.size() >= extension.size() &&
                name.compare(name.size() - extension.size(), extension.size(), extension) == 0) return true;
        }
        start = end + 1;
    }
    return false;
}

std::string ShareStreamRedactor::append(std::string_view delta) {
    pending_.append(delta);
    auto end = pending_.find_last_of('\n');
    if (end == std::string::npos) return {};
    const auto begin_key = pending_.find("-----BEGIN");
    if (begin_key != std::string::npos && pending_.find("-----END", begin_key) == std::string::npos) {
        if (begin_key == 0) return {};
        end = pending_.rfind('\n', begin_key - 1);
        if (end == std::string::npos) return {};
    }
    const auto text = pending_.substr(0, end + 1);
    pending_.erase(0, end + 1);
    return redact_secrets(text).text;
}

std::string ShareStreamRedactor::finish() {
    auto text = std::move(pending_);
    pending_.clear();
    return redact_secrets(text).text;
}
} // namespace acecode::share
