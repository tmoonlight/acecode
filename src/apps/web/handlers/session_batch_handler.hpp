#pragma once

// 侧栏多选批量操作(POST /api/session-batch)的纯函数层:请求解析与置顶顺序计算。
// 曾经前端对每个会话逐个调用单项接口(置顶一次 2 个请求、归档还要再拉一次列表),
// 选 20 个就是几十个往返;批量接口一次请求做完,并把置顶文件 / 全局顺序 / 已读状态
// 的落盘合并成每个文件一次。磁盘与会话状态的读写在 routes_session_batch.cpp。

#include "web/handlers/pinned_sessions_handler.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace acecode::web {

inline constexpr const char* kNoWorkspacePinScope = "__no_workspace__";
// 单次请求的上限。侧栏一屏最多几百行,超过就是异常请求。
inline constexpr std::size_t kMaxSessionBatchItems = 500;

enum class SessionBatchAction { Pin, Unpin, Archive, Unarchive, Read, Unread };

struct SessionBatchItem {
    std::string session_id;
    // 会话所属工作区;无工作区任务为空串并置 no_workspace。
    std::string workspace_hash;
    bool no_workspace = false;
    // 仅 read 使用:客户端已看到的事件游标,与 WS mark_session_read 同义。
    std::uint64_t cursor = 0;

    // 置顶存储的作用域:工作区 hash 或 "__no_workspace__"。
    std::string pin_scope() const {
        return no_workspace ? std::string(kNoWorkspacePinScope) : workspace_hash;
    }
};

struct SessionBatchRequest {
    SessionBatchAction action = SessionBatchAction::Pin;
    // 保持请求顺序(即侧栏可见顺序);同一会话重复出现只保留第一次。
    std::vector<SessionBatchItem> items;
};

const char* session_batch_action_name(SessionBatchAction action);

// 解析请求体。失败返回 false 并写 error(调用方回 400)。会话 id 只接受
// [A-Za-z0-9_-]、长度 1..128,与置顶存储的 id 规则一致。
bool parse_session_batch_request(const nlohmann::json& body,
                                 SessionBatchRequest& out,
                                 std::string& error);

// 一个作用域内的置顶 id 列表应用一批置顶 / 取消置顶。
// 置顶:尚未置顶的会话按请求顺序整体插到最前(请求里第一个排在最上面),
// 已经置顶的保持原位置不动 —— 与逐个点「置顶」的结果一致,不会把已置顶的会话
// 挤乱。取消置顶:全部移除。
std::vector<std::string> apply_batch_pin_session_ids(
    const std::vector<std::string>& current,
    const std::vector<std::string>& batch_ids,
    bool pin);

// 跨作用域的全局置顶视觉顺序,规则同上。pin 时 newly_pinned 只应包含本次
// 新置顶的项(原本就置顶的会话不移动)。
std::vector<PinnedSessionOrderItem> apply_batch_pin_order_items(
    const std::vector<PinnedSessionOrderItem>& current,
    const std::vector<PinnedSessionOrderItem>& batch_items,
    bool pin);

} // namespace acecode::web
