#include "channels/core/state_log.hpp"

#include <cstdint>
#include <map>

namespace acecode::channels::core {
namespace {

const nlohmann::json& field(const nlohmann::json& object, const char* key) {
    static const nlohmann::json kNull;
    if (!object.is_object()) return kNull;
    const auto it = object.find(key);
    return it == object.end() ? kNull : *it;
}

std::string text_of(const nlohmann::json& object, const char* key) {
    const auto& value = field(object, key);
    return value.is_string() ? value.get<std::string>() : std::string{};
}

std::int64_t number_of(const nlohmann::json& object, const char* key) {
    const auto& value = field(object, key);
    return value.is_number_integer() ? value.get<std::int64_t>() : 0;
}

bool flag_of(const nlohmann::json& object, const char* key) {
    const auto& value = field(object, key);
    return value.is_boolean() && value.get<bool>();
}

std::string dump(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::map<std::string, nlohmann::json> index_by(const nlohmann::json& list, const char* key) {
    std::map<std::string, nlohmann::json> out;
    if (!list.is_array()) return out;
    for (const auto& item : list) {
        const auto id = text_of(item, key);
        if (!id.empty()) out[id] = item;
    }
    return out;
}

// 状态 + 机器人名 + 托管进程 + 原因,例如 "connected @ace_bot"、"failed (retry stopped): token 无效"。
std::string state_text(const nlohmann::json& snapshot) {
    std::string text = text_of(snapshot, "state");
    if (text.empty()) text = "unknown";
    if (flag_of(snapshot, "retry_stopped")) text += " (retry stopped)";
    const auto name = text_of(snapshot, "display_name");
    if (!name.empty()) text += " " + name;
    const auto pid = number_of(snapshot, "hosted_by_pid");
    if (pid > 0) text += " (held by pid " + std::to_string(pid) + ")";
    const auto detail = text_of(snapshot, "detail");
    if (!detail.empty()) text += ": " + detail;
    return text;
}

LogLevel state_level(const nlohmann::json& snapshot) {
    const auto state = text_of(snapshot, "state");
    return state == "failed" || state == "retrying" || state == "error" ? LogLevel::Warn : LogLevel::Info;
}

std::string who(const std::string& principal, const std::string& name) {
    return name.empty() ? principal : principal + " (" + name + ")";
}

std::string owner_text(const nlohmann::json& snapshot) {
    const auto owner = text_of(snapshot, "owner");
    if (owner.empty()) return "none";
    const auto& contacts = field(snapshot, "contacts");
    if (contacts.is_array())
        for (const auto& contact : contacts)
            if (text_of(contact, "principal") == owner) return who(owner, text_of(contact, "name"));
    return owner;
}

std::string binding_text(const nlohmann::json& binding) {
    std::string text = text_of(binding, "label");
    const auto chat = text_of(binding, "chat");
    if (!chat.empty()) text += " chat " + chat;
    const auto sender = text_of(binding, "sender");
    if (!sender.empty() && sender != chat) text += " sender " + sender;
    return text;
}

std::string binding_target(const nlohmann::json& binding) {
    std::string text = "session " + text_of(binding, "session_id");
    const auto cwd = text_of(binding, "cwd");
    const auto& no_workspace = field(binding, "no_workspace");
    if (no_workspace.is_boolean() && !no_workspace.get<bool>() && !cwd.empty()) text += " in " + cwd;
    else text += " (no workspace)";
    return text;
}

std::string counts_text(const nlohmann::json& binding) {
    return "sent " + std::to_string(number_of(binding, "sent")) + ", failed " +
           std::to_string(number_of(binding, "failed")) + ", dropped " + std::to_string(number_of(binding, "dropped"));
}

void describe_extra(const nlohmann::json& before, const nlohmann::json& after, bool first,
                    std::vector<StateLogLine>& lines) {
    const auto& extra_before = field(before, "extra");
    const auto& extra_after = field(after, "extra");
    const bool has_after = extra_after.is_object() && !extra_after.empty();
    if (first ? !has_after : extra_before == extra_after) return;
    lines.push_back({LogLevel::Info, "status " + (extra_after.is_object() ? dump(extra_after) : std::string("{}"))});
    const auto& privacy_before = field(extra_before, "privacy_mode");
    const auto& privacy_after = field(extra_after, "privacy_mode");
    if (privacy_after.is_boolean() && privacy_after != privacy_before) {
        lines.push_back({LogLevel::Info, privacy_after.get<bool>()
            ? "group privacy mode on: groups deliver only @mentions, replies to the bot and commands"
            : "group privacy mode off: the bot receives every group message but only handles @mentions"});
    }
    if (flag_of(extra_after, "webhook") && !flag_of(extra_before, "webhook")) {
        lines.push_back({LogLevel::Warn,
                         "a webhook is set for this bot; long polling stays blocked until it is removed in the connect dialog"});
    }
    const auto held = number_of(extra_after, "held");
    if (held != number_of(extra_before, "held") && held > 0) {
        lines.push_back({LogLevel::Warn, std::to_string(held) +
                         " output(s) held after QQ refused them; they are resent before the contact's next message"});
    }
}

void describe_access(const nlohmann::json& before, const nlohmann::json& after, bool first,
                     std::vector<StateLogLine>& lines) {
    const bool configured = flag_of(after, "configured");
    if (first ? configured : text_of(before, "owner") != text_of(after, "owner"))
        lines.push_back({LogLevel::Info, "owner " + owner_text(after)});
    if (flag_of(after, "owner_window") != flag_of(before, "owner_window")) {
        lines.push_back({LogLevel::Info, flag_of(after, "owner_window")
            ? "owner window open: the first private chat within 10 minutes becomes the owner"
            : "owner window closed"});
    }
    const auto contacts_before = index_by(field(before, "contacts"), "principal");
    const auto contacts_after = index_by(field(after, "contacts"), "principal");
    for (const auto& [principal, contact] : contacts_after) {
        if (contacts_before.count(principal)) continue;
        lines.push_back({LogLevel::Info, (first ? "authorized " : "access granted: ") +
                                             who(principal, text_of(contact, "name"))});
    }
    for (const auto& [principal, contact] : contacts_before) {
        if (contacts_after.count(principal)) continue;
        lines.push_back({LogLevel::Info, "access removed: " + who(principal, text_of(contact, "name"))});
    }
    const auto pending_before = index_by(field(before, "pending"), "id");
    const auto pending_after = index_by(field(after, "pending"), "id");
    for (const auto& [id, request] : pending_after) {
        if (pending_before.count(id)) continue;
        lines.push_back({LogLevel::Info, "request " + id + " pending: " + text_of(request, "kind") + " " +
                                             who(text_of(request, "principal"), text_of(request, "name")) + " via " +
                                             text_of(request, "label") + ", expires in " +
                                             std::to_string(number_of(request, "expires_in_s")) + "s"});
    }
    for (const auto& [id, request] : pending_before) {
        if (pending_after.count(id)) continue;
        lines.push_back({LogLevel::Info, "request " + id + " closed (approved, rejected or expired): " +
                                             who(text_of(request, "principal"), text_of(request, "name"))});
    }
}

void describe_bindings(const nlohmann::json& before, const nlohmann::json& after, bool first,
                       std::vector<StateLogLine>& lines) {
    const auto bindings_before = index_by(field(before, "bindings"), "key");
    const auto bindings_after = index_by(field(after, "bindings"), "key");
    for (const auto& [key, binding] : bindings_after) {
        const auto old = bindings_before.find(key);
        if (old == bindings_before.end()) {
            lines.push_back({LogLevel::Info, "binding " + binding_text(binding) + " -> " + binding_target(binding) +
                                                 (first ? " (" + counts_text(binding) + ")" : std::string{})});
            continue;
        }
        if (text_of(old->second, "session_id") != text_of(binding, "session_id"))
            lines.push_back({LogLevel::Info, "binding " + binding_text(binding) + " -> " + binding_target(binding)});
        const auto sent = number_of(binding, "sent") - number_of(old->second, "sent");
        const auto failed = number_of(binding, "failed") - number_of(old->second, "failed");
        const auto dropped = number_of(binding, "dropped") - number_of(old->second, "dropped");
        if (failed > 0)
            lines.push_back({LogLevel::Warn, "delivery to " + binding_text(binding) + " failed (" + counts_text(binding) + ")"});
        if (dropped > 0)
            lines.push_back({LogLevel::Warn, std::to_string(dropped) + " output(s) to " + binding_text(binding) +
                                                 " dropped (" + counts_text(binding) + ")"});
        if (sent > 0)
            lines.push_back({LogLevel::Dbg, "delivered " + std::to_string(sent) + " to " + binding_text(binding) + " (" +
                                                counts_text(binding) + ")"});
    }
    for (const auto& [key, binding] : bindings_before) {
        if (bindings_after.count(key)) continue;
        lines.push_back({LogLevel::Info, "binding removed: " + binding_text(binding) + " (was " +
                                             binding_target(binding) + ")"});
    }
}

} // namespace

std::vector<StateLogLine> describe_state_changes(const nlohmann::json& previous, const nlohmann::json& next) {
    // 首次发布时 previous 不是对象,下面的取值一律得到空值,相当于和空快照比较。
    std::vector<StateLogLine> lines;
    const bool first = !previous.is_object();
    const auto after_state = state_text(next);
    if (first) {
        lines.push_back({state_level(next), "state " + after_state});
    } else {
        const auto before_state = state_text(previous);
        if (before_state != after_state)
            lines.push_back({state_level(next), "state " + before_state + " -> " + after_state});
        if (text_of(previous, "credential_hint") != text_of(next, "credential_hint") ||
            text_of(previous, "app_id") != text_of(next, "app_id") ||
            field(previous, "credentials_public") != field(next, "credentials_public")) {
            const auto app_id = text_of(next, "app_id");
            lines.push_back({LogLevel::Info, app_id.empty() ? std::string("credentials updated")
                                                             : "credentials updated (AppID " + app_id + ")"});
        }
    }
    describe_extra(previous, next, first, lines);
    describe_access(previous, next, first, lines);
    describe_bindings(previous, next, first, lines);
    return lines;
}

} // namespace acecode::channels::core
