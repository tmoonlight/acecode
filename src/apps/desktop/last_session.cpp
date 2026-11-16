#include "last_session.hpp"

#include "url_builder.hpp"

#include "utils/state_file.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <system_error>

namespace acecode::desktop {
namespace {

constexpr const char* kStateKey = "last_session";
constexpr std::size_t kMaxIdentityBytes = 256;

// 两个字段最终都进 URL 查询串(已 percent-encode)。这里只挡明显不是 id 的内容:
// 过长,或带控制字符。
bool valid_identity(const std::string& value) {
    if (value.size() > kMaxIdentityBytes) return false;
    for (const unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7F) return false;
    }
    return true;
}

std::optional<LastSession> session_from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto text = [&value](const char* key) -> std::string {
        const auto it = value.find(key);
        return it != value.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    const auto flag = [&value](const char* key) {
        const auto it = value.find(key);
        return it != value.end() && it->is_boolean() && it->get<bool>();
    };

    LastSession session;
    session.session_id = text("session_id");
    session.no_workspace = flag("no_workspace");
    session.workspace_hash = session.no_workspace ? std::string{} : text("workspace_hash");
    session.read_only = flag("read_only");
    if (session.session_id.empty() || !valid_identity(session.session_id) ||
        !valid_identity(session.workspace_hash)) {
        return std::nullopt;
    }
    return session;
}

nlohmann::json session_to_json(const LastSession& session) {
    nlohmann::json value{{"session_id", session.session_id}};
    if (session.no_workspace) value["no_workspace"] = true;
    else if (!session.workspace_hash.empty()) value["workspace_hash"] = session.workspace_hash;
    if (session.read_only) value["read_only"] = true;
    return value;
}

std::string error_json(const std::string& error) {
    return nlohmann::json{{"ok", false}, {"error", error}}.dump();
}

} // namespace

std::optional<LastSession> read_last_session() {
    const nlohmann::json state = read_state_json();
    const auto it = state.find(kStateKey);
    return it == state.end() ? std::nullopt : session_from_json(*it);
}

void write_last_session(const std::optional<LastSession>& session) {
    (void)update_state_json([&session](nlohmann::json& state) {
        const auto it = state.find(kStateKey);
        if (!session) {
            if (it == state.end()) return false;
            state.erase(it);
            return true;
        }
        nlohmann::json next = session_to_json(*session);
        if (it != state.end() && *it == next) return false;
        state[kStateKey] = std::move(next);
        return true;
    });
}

std::optional<LastSession> restorable_last_session(const WorkspaceRegistry& registry) {
    auto session = read_last_session();
    if (!session || session->workspace_hash.empty()) return session;
    const auto workspace = registry.get(session->workspace_hash);
    if (!workspace) return std::nullopt;
    std::error_code ec;
    if (!std::filesystem::is_directory(path_from_utf8(workspace->cwd), ec) || ec) {
        return std::nullopt;
    }
    return session;
}

std::string last_session_url(const std::string& url, const LastSession& session) {
    std::string next = append_query_param(url, "open", session.session_id);
    if (session.no_workspace) {
        next = append_query_param(next, "no_workspace", "1");
    } else if (!session.workspace_hash.empty()) {
        next = append_query_param(next, "workspace", session.workspace_hash);
    }
    if (session.read_only) next = append_query_param(next, "read_only", "1");
    return append_query_param(next, "restore", "1");
}

std::string handle_set_last_session(const std::string& request) {
    try {
        const auto args = nlohmann::json::parse(request);
        if (!args.is_array() || args.empty()) return error_json("expect [session|null]");
        if (args[0].is_null()) {
            write_last_session(std::nullopt);
            return nlohmann::json{{"ok", true}}.dump();
        }
        const auto session = session_from_json(args[0]);
        if (!session) return error_json("invalid session");
        write_last_session(session);
        return nlohmann::json{{"ok", true}}.dump();
    } catch (const std::exception& e) {
        return error_json(std::string("parse: ") + e.what());
    }
}

} // namespace acecode::desktop
