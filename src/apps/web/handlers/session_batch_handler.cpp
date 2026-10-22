#include "web/handlers/session_batch_handler.hpp"

#include <algorithm>
#include <unordered_set>

namespace acecode::web {

using nlohmann::json;

namespace {

bool valid_session_id(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
}

bool parse_action(const std::string& name, SessionBatchAction& out) {
    static const std::pair<const char*, SessionBatchAction> kActions[] = {
        {"pin", SessionBatchAction::Pin},
        {"unpin", SessionBatchAction::Unpin},
        {"archive", SessionBatchAction::Archive},
        {"unarchive", SessionBatchAction::Unarchive},
        {"read", SessionBatchAction::Read},
        {"unread", SessionBatchAction::Unread},
    };
    for (const auto& [key, action] : kActions) {
        if (name == key) {
            out = action;
            return true;
        }
    }
    return false;
}

std::string order_key(const PinnedSessionOrderItem& item) {
    return item.workspace_hash + '\0' + item.session_id;
}

} // namespace

const char* session_batch_action_name(SessionBatchAction action) {
    switch (action) {
        case SessionBatchAction::Pin: return "pin";
        case SessionBatchAction::Unpin: return "unpin";
        case SessionBatchAction::Archive: return "archive";
        case SessionBatchAction::Unarchive: return "unarchive";
        case SessionBatchAction::Read: return "read";
        case SessionBatchAction::Unread: return "unread";
    }
    return "";
}

bool parse_session_batch_request(const json& body,
                                 SessionBatchRequest& out,
                                 std::string& error) {
    out = SessionBatchRequest{};
    if (!body.is_object()) {
        error = "request body must be an object";
        return false;
    }
    if (!body.contains("action") || !body["action"].is_string() ||
        !parse_action(body["action"].get<std::string>(), out.action)) {
        error = "action must be one of pin, unpin, archive, unarchive, read, unread";
        return false;
    }
    if (!body.contains("items") || !body["items"].is_array() || body["items"].empty()) {
        error = "items must be a non-empty array";
        return false;
    }
    if (body["items"].size() > kMaxSessionBatchItems) {
        error = "too many items (max " + std::to_string(kMaxSessionBatchItems) + ")";
        return false;
    }

    std::unordered_set<std::string> seen;
    for (const auto& raw : body["items"]) {
        if (!raw.is_object()) {
            error = "each item must be an object";
            return false;
        }
        SessionBatchItem item;
        if (raw.contains("session_id") && raw["session_id"].is_string()) {
            item.session_id = raw["session_id"].get<std::string>();
        }
        if (!valid_session_id(item.session_id)) {
            error = "invalid session_id";
            return false;
        }
        if (raw.contains("workspace_hash") && raw["workspace_hash"].is_string()) {
            item.workspace_hash = raw["workspace_hash"].get<std::string>();
        }
        if (raw.contains("no_workspace") && raw["no_workspace"].is_boolean()) {
            item.no_workspace = raw["no_workspace"].get<bool>();
        }
        // 前端的置顶作用域写法(workspace_hash = "__no_workspace__")也接受。
        if (item.workspace_hash == kNoWorkspacePinScope) {
            item.no_workspace = true;
            item.workspace_hash.clear();
        }
        if (item.no_workspace) item.workspace_hash.clear();
        // 浏览器发来的 JSON 整数在 nlohmann 里可能是有符号类型,只要非负就接受。
        if (raw.contains("cursor") && raw["cursor"].is_number_integer() &&
            (raw["cursor"].is_number_unsigned() || raw["cursor"].get<std::int64_t>() >= 0)) {
            item.cursor = raw["cursor"].get<std::uint64_t>();
        }
        if (!seen.insert(item.pin_scope() + '\0' + item.session_id).second) continue;
        out.items.push_back(std::move(item));
    }
    return true;
}

std::vector<std::string> apply_batch_pin_session_ids(
    const std::vector<std::string>& current,
    const std::vector<std::string>& batch_ids,
    bool pin) {
    const auto normalized = normalize_pinned_session_ids(current);
    std::unordered_set<std::string> batch;
    for (const auto& id : batch_ids) {
        if (!id.empty()) batch.insert(id);
    }
    if (!pin) {
        std::vector<std::string> out;
        for (const auto& id : normalized) {
            if (!batch.count(id)) out.push_back(id);
        }
        return out;
    }
    const std::unordered_set<std::string> existing(normalized.begin(), normalized.end());
    std::vector<std::string> out;
    std::unordered_set<std::string> added;
    for (const auto& id : batch_ids) {
        if (id.empty() || existing.count(id) || !added.insert(id).second) continue;
        out.push_back(id);
    }
    out.insert(out.end(), normalized.begin(), normalized.end());
    return out;
}

std::vector<PinnedSessionOrderItem> apply_batch_pin_order_items(
    const std::vector<PinnedSessionOrderItem>& current,
    const std::vector<PinnedSessionOrderItem>& batch_items,
    bool pin) {
    const auto normalized = normalize_pinned_session_order_items(current);
    std::unordered_set<std::string> batch;
    for (const auto& item : batch_items) {
        if (!item.workspace_hash.empty() && !item.session_id.empty()) batch.insert(order_key(item));
    }
    if (!pin) {
        std::vector<PinnedSessionOrderItem> out;
        for (const auto& item : normalized) {
            if (!batch.count(order_key(item))) out.push_back(item);
        }
        return out;
    }
    std::unordered_set<std::string> existing;
    for (const auto& item : normalized) existing.insert(order_key(item));
    std::vector<PinnedSessionOrderItem> out;
    std::unordered_set<std::string> added;
    for (const auto& item : batch_items) {
        if (item.workspace_hash.empty() || item.session_id.empty()) continue;
        const auto key = order_key(item);
        if (existing.count(key) || !added.insert(key).second) continue;
        out.push_back(item);
    }
    out.insert(out.end(), normalized.begin(), normalized.end());
    return out;
}

} // namespace acecode::web
