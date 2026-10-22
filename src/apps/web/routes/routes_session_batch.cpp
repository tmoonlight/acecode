// routes_session_batch.cpp — 侧栏多选批量操作 POST /api/session-batch。
// 置顶 / 取消置顶 / 归档 / 取消归档 / 已读 / 未读一次请求做完:每个置顶文件、
// 全局置顶顺序文件、每个 workspace 的 attention 文件各只写一次。单项失败不影响
// 其它项,整体仍回 200,逐项结果在 results 里。协议见 docs/daemon-api.md。
#ifdef DELETE
#undef DELETE
#endif
#include "web/server_impl.hpp"
#include "web/handlers/session_batch_handler.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_map>

namespace acecode::web {

using nlohmann::json;

namespace {

json batch_item_json(const SessionBatchItem& item) {
    return json{
        {"session_id", item.session_id},
        {"workspace_hash", item.workspace_hash},
        {"no_workspace", item.no_workspace},
    };
}

void mark_failed(json& result, const std::string& error) {
    result["ok"] = false;
    result["error"] = error;
}

bool contains_id(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// 按置顶作用域分组,保持首次出现的顺序与组内的请求顺序。
std::vector<std::pair<std::string, std::vector<std::size_t>>> group_by_pin_scope(
    const std::vector<SessionBatchItem>& items,
    const std::function<bool(std::size_t)>& include) {
    std::vector<std::pair<std::string, std::vector<std::size_t>>> groups;
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!include(i)) continue;
        const auto scope = items[i].pin_scope();
        auto [it, inserted] = index.emplace(scope, groups.size());
        if (inserted) groups.push_back({scope, {}});
        groups[it->second].second.push_back(i);
    }
    return groups;
}

} // namespace

void WebServer::Impl::register_session_batch() {
    CROW_ROUTE(app, "/api/session-batch").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) {
        return cors_preflight(req);
    });

    CROW_ROUTE(app, "/api/session-batch").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        return handle_session_batch(req);
    });
}

crow::response WebServer::Impl::handle_session_batch(const crow::request& req) {
    const auto reply = [this, &req](int status, const json& body) {
        crow::response r(status);
        r.body = body.dump();
        r.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(r));
    };

    SessionBatchRequest batch;
    try {
        const auto body = json::parse(req.body.empty() ? "{}" : req.body);
        std::string error;
        if (!parse_session_batch_request(body, batch, error)) {
            return reply(400, json{{"error", error}});
        }
    } catch (const std::exception& e) {
        return reply(400, json{{"error", std::string("bad json: ") + e.what()}});
    }

    const auto& items = batch.items;
    std::vector<json> results;
    results.reserve(items.size());
    for (const auto& item : items) {
        auto result = batch_item_json(item);
        result["ok"] = true;
        results.push_back(std::move(result));
    }

    json pinned_scopes = json::array();
    std::optional<json> pinned_order;

    // 读取某个置顶作用域的文件路径与「哪些 id 可以置顶」的判定;作用域不存在返回 false。
    const auto pin_scope_store = [this](const std::string& scope,
                                        std::filesystem::path& path,
                                        std::function<std::vector<std::string>(
                                            const std::vector<std::string>&)>& available) {
        if (scope == kNoWorkspacePinScope) {
            path = no_workspace_pinned_sessions_path();
            available = [this](const std::vector<std::string>& ids) {
                return session_ids_for_no_workspace(ids);
            };
            return true;
        }
        auto ws = scope.empty() ? std::nullopt : resolve_workspace(scope);
        if (!ws.has_value()) return false;
        path = pinned_sessions_path_for_cwd(ws->cwd);
        available = [this, ws = *ws](const std::vector<std::string>& ids) {
            return session_ids_for_workspace(ws, ids);
        };
        return true;
    };

    // 从全局置顶顺序里加入 / 移除一批项,只在内容变化时写盘。
    const auto update_pinned_order = [this, &pinned_order](
                                         const std::vector<PinnedSessionOrderItem>& changed_items,
                                         bool pin) {
        const auto path = pinned_session_order_path();
        const auto state = read_pinned_session_order_state(path);
        auto next = apply_batch_pin_order_items(state.items, changed_items, pin);
        if (pin) {
            // 与 PUT /api/pinned-sessions/order 一致:只保留各作用域里确实置顶的项。
            next = prune_pinned_session_order_items(next, available_pinned_session_order_items());
        }
        if (next != state.items) {
            std::string error;
            if (!write_pinned_session_order_state(path, PinnedSessionOrderState{next}, &error)) {
                LOG_WARN("[web] session batch failed to write pinned order: " + error);
                next = state.items;
            }
        }
        pinned_order = pinned_session_order_to_json(next);
    };

    switch (batch.action) {
        case SessionBatchAction::Pin:
        case SessionBatchAction::Unpin: {
            const bool pin = batch.action == SessionBatchAction::Pin;
            // 全局顺序按请求顺序(侧栏可见顺序)排,不能按作用域分组后的顺序。
            std::vector<bool> order_touched(items.size(), false);
            for (const auto& [scope, indexes] : group_by_pin_scope(items, [](std::size_t) { return true; })) {
                std::filesystem::path path;
                std::function<std::vector<std::string>(const std::vector<std::string>&)> available;
                if (!pin_scope_store(scope, path, available)) {
                    for (const auto i : indexes) mark_failed(results[i], "workspace not found");
                    continue;
                }
                std::vector<std::string> ids;
                for (const auto i : indexes) ids.push_back(items[i].session_id);
                const auto state = read_pinned_sessions_state(path);
                const auto next = apply_batch_pin_session_ids(state.session_ids, ids, pin);
                const auto pruned = prune_pinned_session_ids(next, available(next));
                if (pruned != state.session_ids) {
                    std::string error;
                    if (!write_pinned_sessions_state(path, PinnedSessionsState{pruned}, &error)) {
                        for (const auto i : indexes) {
                            mark_failed(results[i], "failed to write pinned sessions: " + error);
                        }
                        continue;
                    }
                }
                for (const auto i : indexes) {
                    const bool pinned_now = contains_id(pruned, items[i].session_id);
                    if (pin && !pinned_now) {
                        // 不存在、已归档或子会话:置顶存储会把它剪掉。
                        mark_failed(results[i], "session cannot be pinned");
                        continue;
                    }
                    order_touched[i] = true;
                }
                pinned_scopes.push_back(json{{"pin_scope", scope}, {"session_ids", pruned}});
            }
            std::vector<PinnedSessionOrderItem> order_items;
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (order_touched[i]) {
                    order_items.push_back(PinnedSessionOrderItem{items[i].pin_scope(), items[i].session_id});
                }
            }
            if (!pinned_scopes.empty()) update_pinned_order(order_items, pin);
            break;
        }
        case SessionBatchAction::Archive:
        case SessionBatchAction::Unarchive: {
            const bool archive = batch.action == SessionBatchAction::Archive;
            for (std::size_t i = 0; i < items.size(); ++i) {
                const auto& item = items[i];
                // 与单项路由相同:无工作区任务与未知工作区走兼容 workspace。
                std::optional<acecode::desktop::WorkspaceMeta> ws;
                if (item.no_workspace || item.workspace_hash.empty() ||
                    item.workspace_hash == "__local__") {
                    ws = compatibility_workspace();
                } else {
                    ws = resolve_workspace(item.workspace_hash);
                }
                if (!ws.has_value()) {
                    mark_failed(results[i], "workspace not found");
                    continue;
                }
                auto outcome = apply_session_archive_state(*ws, item.session_id, archive);
                if (outcome.status != 200) {
                    mark_failed(results[i], outcome.body.value("error", std::string("archive failed")));
                    continue;
                }
                results[i]["session"] = std::move(outcome.body);
            }
            if (!archive) break;
            // 已归档的会话从置顶里移除:每个作用域一次写盘,全局顺序一次写盘。
            std::vector<PinnedSessionOrderItem> order_items;
            const auto archived_ok = [&results](std::size_t i) {
                return results[i].value("ok", false);
            };
            for (const auto& [scope, indexes] : group_by_pin_scope(items, archived_ok)) {
                std::filesystem::path path;
                std::function<std::vector<std::string>(const std::vector<std::string>&)> available;
                if (!pin_scope_store(scope, path, available)) continue;
                std::vector<std::string> ids;
                for (const auto i : indexes) {
                    ids.push_back(items[i].session_id);
                    order_items.push_back(PinnedSessionOrderItem{scope, items[i].session_id});
                }
                const auto state = read_pinned_sessions_state(path);
                const auto next = apply_batch_pin_session_ids(state.session_ids, ids, false);
                if (next != state.session_ids) {
                    std::string error;
                    if (!write_pinned_sessions_state(path, PinnedSessionsState{next}, &error)) {
                        // 归档已经成功;残留的置顶会在下次读取时按归档状态剪掉。
                        LOG_WARN("[web] session batch archive failed to unpin: " + error);
                        continue;
                    }
                }
                pinned_scopes.push_back(json{{"pin_scope", scope}, {"session_ids", next}});
            }
            if (!order_items.empty()) update_pinned_order(order_items, false);
            break;
        }
        case SessionBatchAction::Read:
        case SessionBatchAction::Unread: {
            std::vector<SessionAttentionMark> marks;
            marks.reserve(items.size());
            for (const auto& item : items) {
                // 与 WS mark_session_read / mark_session_unread 同一解析口径。
                const auto ws = resolve_session_workspace(
                    item.session_id, item.no_workspace ? std::string{} : item.workspace_hash);
                marks.push_back(SessionAttentionMark{
                    item.session_id,
                    ws ? ws->hash : item.workspace_hash,
                    ws ? ws->cwd : std::string{},
                    item.cursor,
                });
            }
            auto statuses = mark_sessions_attention(
                marks, batch.action == SessionBatchAction::Read);
            for (std::size_t i = 0; i < statuses.size(); ++i) {
                results[i]["status"] = std::move(statuses[i]);
            }
            break;
        }
    }

    std::size_t succeeded = 0;
    json results_json = json::array();
    for (auto& result : results) {
        if (result.value("ok", false)) ++succeeded;
        results_json.push_back(std::move(result));
    }
    json body{
        {"action", session_batch_action_name(batch.action)},
        {"results", std::move(results_json)},
        {"succeeded", succeeded},
        {"failed", items.size() - succeeded},
    };
    if (!pinned_scopes.empty()) body["pinned_scopes"] = std::move(pinned_scopes);
    if (pinned_order) body["pinned_order"] = std::move(*pinned_order);
    return reply(200, body);
}

} // namespace acecode::web
