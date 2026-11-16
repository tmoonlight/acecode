#pragma once

// 桌面版「启动时回到上次停留的会话」。
//
// 前端每次切换当前会话时经 aceDesktop_setLastSession 上报,这里记进
// state.json::last_session;停在首页 / 定时任务页等非会话页面时上报 null,
// 记录随之清掉(下次启动就落在首页)。启动时 main.cpp 读回记录,拼成与
// 跨 workspace 跳转相同的 ?open=<会话>&workspace=<hash> 落地参数,交给前端
// 已有的启动跳转去恢复。
//
// 记录只由桌面壳读写:端口每次启动都变,浏览器本地存储留不住;daemon 可能同时
// 服务多个页面,不适合记「这一个窗口」停在哪。

#include "workspace/workspace_registry.hpp"

#include <optional>
#include <string>

namespace acecode::desktop {

struct LastSession {
    std::string session_id;
    std::string workspace_hash; // no_workspace 时恒为空
    bool no_workspace = false;  // 明确不属于任何工作区的会话
    bool read_only = false;     // 只读打开的外部会话(TUI)

    bool operator==(const LastSession& other) const {
        return session_id == other.session_id &&
               workspace_hash == other.workspace_hash &&
               no_workspace == other.no_workspace &&
               read_only == other.read_only;
    }
};

// 读: 文件不存在 / 字段缺失 / 类型或内容不合法 → nullopt。永不抛异常。
std::optional<LastSession> read_last_session();

// 写: nullopt 清掉记录;内容没变不落盘。失败只 LOG_WARN 不阻断。
void write_last_session(const std::optional<LastSession>& session);

// 本次启动要恢复的会话。带工作区的会话要求该工作区仍在 registry 里且目录还在
// (否则 daemon 那边无从 resume);没有工作区信息的会话总是交给前端去试。
std::optional<LastSession> restorable_last_session(const WorkspaceRegistry& registry);

// 给 daemon 首页 URL 追加启动落地参数。restore=1 告诉前端这是自动恢复:
// 会话已被删除等失败情况静默回到首页,不弹错误。
std::string last_session_url(const std::string& url, const LastSession& session);

// bridge aceDesktop_setLastSession 的处理函数。请求是参数数组:
// [null] 或 [{session_id, workspace_hash?, no_workspace?, read_only?}]。
// 返回 {"ok":true} 或 {"ok":false,"error":...}。
std::string handle_set_last_session(const std::string& request);

} // namespace acecode::desktop
