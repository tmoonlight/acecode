#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace acecode::desktop {

// Agent Browser 的 cookie 存储状态,落在 state.json 的 `agent_browser` 段。
// Desktop(持有 WebView2 / WebKit 存储)与 daemon / TUI(会话的删除、分叉发生在
// 这里)都要读写它,所以放在 base/ipc,两边共用同一份读写与校验规则。
//
// 写入归属(openspec add-agent-browser-cookie-modes D6):
//   cookie_mode / session_stores / pending_wipe / shared_wipe_pending 只由 Desktop 写;
//   pending_deletions / pending_seeds 由 daemon / TUI 追加,由 Desktop 消费。
// 本模块只保证格式与上限,不做存储操作。

inline constexpr const char kAgentBrowserStoreStateKey[] = "agent_browser";
// pending_seeds 上限:shared / until_quit 档下这些记录永远不会被消费,超出丢最旧的。
inline constexpr std::size_t kAgentBrowserMaxPendingSeeds = 500;
// 其它清单的上限只防异常膨胀(正常远小于此值),超出丢最旧的。
inline constexpr std::size_t kAgentBrowserMaxStoreListEntries = 20000;

struct AgentBrowserStoreSeed {
    std::string session_id;  // 分叉出的会话
    std::string from;        // 父会话
    std::int64_t at_ms = 0;
};

struct AgentBrowserStoreState {
    // 用户选过的档位原文;空 = 从未选过(按 shared 处理)。未知值原样保留,
    // 由使用方按 shared 处理,直到用户重新选择。
    std::string cookie_mode;
    // 磁盘上已经建立过的按会话存储(根会话 id)。切到「退出即清」时据此枚举。
    std::vector<std::string> session_stores;
    // 待清空的按会话存储:下次启动在建 environment 之前删除磁盘目录,本次运行里
    // 若又被用到,先清空再用。
    std::vector<std::string> pending_wipe;
    // 共享存储还欠一次清空(切到「退出即清」时立即清空;清空完成才清掉这个标记,
    // 失败或异常退出时下次启动补清)。
    bool shared_wipe_pending = false;
    // 已永久删除的会话,其存储待删除。
    std::vector<std::string> pending_deletions;
    // 分叉记录:子会话第一次建存储时从父会话复制 cookie。
    std::vector<AgentBrowserStoreSeed> pending_seeds;
};

// 会话 id 的字符集与长度(与页面 owner 的 session_id 规则一致):
// [A-Za-z0-9-_.~:@+],1..128 字符。不合法的 id 一律不进状态文件。
bool valid_agent_browser_store_session_id(const std::string& session_id);

// 容错解析:非对象 / 字段类型不符 / 非法 id 的条目丢弃,清单去重(保留首次出现的
// 顺序),超上限时丢最旧的。永不抛异常。
AgentBrowserStoreState agent_browser_store_state_from_json(const nlohmann::json& section);
nlohmann::json agent_browser_store_state_to_json(const AgentBrowserStoreState& state);

// 读 state.json 的 agent_browser 段;文件缺失 / 损坏 / 段缺失都返回空状态。
AgentBrowserStoreState read_agent_browser_store_state();
// 在 state_file 的跨进程锁下读-改-写 agent_browser 段。mutate 返回 false 表示
// 放弃写入(仍视为成功)。拿不到锁或写失败返回 false。写入前统一规范化
// (去重、上限、丢弃非法 id),mutate 不必自己处理。
bool update_agent_browser_store_state(
    const std::function<bool(AgentBrowserStoreState&)>& mutate);

// daemon / TUI 用:记录永久删除的会话(同时撤掉以它为子会话的分叉记录)。
// id 不合法时返回 false 且不写入。
bool record_agent_browser_store_deletion(const std::string& session_id);
// daemon / TUI 用:记录分叉。同一子会话再次记录时以最新一条为准。
bool record_agent_browser_store_seed(const std::string& session_id,
                                     const std::string& from_session_id,
                                     std::int64_t at_ms);

} // namespace acecode::desktop
