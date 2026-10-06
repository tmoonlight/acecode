#include "transport.hpp"

#include <stdexcept>

namespace acecode::im {
namespace {

bool simple_id(const std::string& value, std::size_t limit) {
    if (value.empty() || value.size() > limit) return false;
    for (const char c : value) {
        // 钉钉的会话 id 是 base64(含 + / =),发送人 id 形如 $:LWCP_v1:$…;都不用于文件路径。
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '_' || c == '.' || c == ':' || c == '@' || c == '+' || c == '/' ||
                        c == '=' || c == '$';
        if (!ok) return false;
    }
    return true;
}

} // namespace

std::string Address::key() const {
    nlohmann::json key = nlohmann::json::array({platform, account,
                                                kind == ChatKind::Group ? "group" : "private", chat});
    if (kind == ChatKind::Group) key.push_back(sender);
    return key.dump();
}

nlohmann::json Address::to_json() const {
    nlohmann::json value{{"platform", platform},
                         {"account", account},
                         {"kind", kind == ChatKind::Group ? "group" : "private"},
                         {"chat", chat},
                         {"sender", sender}};
    if (!thread.empty()) value["thread"] = thread;
    return value;
}

Address Address::from_json(const nlohmann::json& value) {
    Address address;
    address.platform = value.at("platform").get<std::string>();
    address.account = value.at("account").get<std::string>();
    const auto kind = value.at("kind").get<std::string>();
    if (kind != "group" && kind != "private") throw std::runtime_error("Invalid channel address kind");
    address.kind = kind == "group" ? ChatKind::Group : ChatKind::Private;
    address.chat = value.at("chat").get<std::string>();
    address.sender = value.at("sender").get<std::string>();
    address.thread = value.value("thread", std::string{});
    return address;
}

bool Address::valid() const {
    static const char* const kPlatforms[] = {"qq", "weixin", "feishu", "dingtalk", "telegram", "discord", "line"};
    bool known = false;
    for (const char* name : kPlatforms) known = known || platform == name;
    if (!known) return false;
    if (!simple_id(account, 128) || !simple_id(chat, 128) || !simple_id(sender, 128)) return false;
    if (!thread.empty() && !simple_id(thread, 64)) return false;
    return kind == ChatKind::Group || chat == sender;
}

const char* link_state_name(LinkState state) {
    switch (state) {
        case LinkState::Stopped: return "stopped";
        case LinkState::Connecting: return "connecting";
        case LinkState::Connected: return "connected";
        case LinkState::Retrying: return "retrying";
        case LinkState::Failed: return "failed";
    }
    return "stopped";
}

} // namespace acecode::im
