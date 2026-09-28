#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode::desktop {

// 一个 Browser 页面属于哪个会话。
//
// `session_id` 是唯一的联结键:daemon 工具、Desktop host 与 Web UI 三方都用它对
// 账。`workspace_hash` 与 `root_session_id` 只是附带信息 —— 前者用于跨工作区导航
// 提示,后者让子代理开的页面在展示上归到父会话;两者都不参与匹配,因为工作区
// hash 在 junction / no-workspace 会话下两侧形态未必一致。
//
// 空 owner(session_id 为空)表示「未绑定会话」:旧版 daemon 发来的请求、或旧版
// Web UI 创建的页面。它们沿用引入归属之前的行为(全局显示页即默认目标)。
struct AgentBrowserPageOwner {
    std::string session_id;
    std::string workspace_hash;
    std::string root_session_id;

    bool empty() const { return session_id.empty(); }
    bool same_session(const AgentBrowserPageOwner& other) const {
        return !empty() && session_id == other.session_id;
    }
};

bool operator==(const AgentBrowserPageOwner& lhs,
                const AgentBrowserPageOwner& rhs);
bool operator!=(const AgentBrowserPageOwner& lhs,
                const AgentBrowserPageOwner& rhs);

// 解析代理请求 / Desktop bridge 参数里的 owner 对象。非对象、缺字段或字段含非法
// 字符时对应字段为空;session_id 为空的结果视为未绑定。字段上限 128 字符,字符集
// 与 headless `--session-id` 允许的集合一致并额外放行 `.` `~` `:` `@` `+`。
AgentBrowserPageOwner parse_agent_browser_page_owner(const nlohmann::json& value);

// 序列化到状态事件与 bridge 结果;空 owner 输出 null,让前端能区分「未绑定」。
nlohmann::json agent_browser_page_owner_json(const AgentBrowserPageOwner& owner);

struct AgentBrowserPageResolution {
    // 非空 = 命中现有页面(调用方仍需校验页面是否存活、是否共享给 Agent)。
    std::string page_id;
    // true = 该 owner 没有可用页面,需要新建一页。
    bool create = false;
};

// Windows / macOS host 共用的页面簿记,不依赖任何平台 API。
//
// 它把引入归属之前的单一「active page」拆成两个概念:
// - displayed page:全局唯一,Web UI 当前显示在详情栏里的页面。只由 UI 的
//   select 请求或关闭回退设置;daemon 为某个会话建页**不**改变它,否则会把用户
//   正在看的别的会话的页面挤掉。
// - agent target:按会话各存一份,工具省略 page_id 时的默认目标。browser_open
//   与显式 select 会更新它。
class AgentBrowserPageDirectory {
public:
    void add_page(const std::string& page_id,
                  const AgentBrowserPageOwner& owner,
                  bool agent_target);
    void remove_page(const std::string& page_id);
    void clear();
    bool contains(const std::string& page_id) const;
    std::size_t size() const { return order_.size(); }
    const std::vector<std::string>& ordered_page_ids() const { return order_; }
    std::vector<std::string> page_ids_for_session(
        const std::string& session_id) const;
    AgentBrowserPageOwner owner_of(const std::string& page_id) const;

    const std::string& displayed_page_id() const { return displayed_; }
    bool set_displayed_page(const std::string& page_id);
    void clear_displayed_page() { displayed_.clear(); }

    std::string agent_target_for(const AgentBrowserPageOwner& owner) const;
    bool set_agent_target(const AgentBrowserPageOwner& owner,
                          const std::string& page_id);
    bool is_agent_target(const std::string& page_id) const;

    // 工具请求落到哪一页:显式 page_id 原样返回;省略时按 owner 解析 ——
    // 有 owner:该会话的 agent target > 当前显示页(若也属于该会话)> 新建;
    // 无 owner(旧协议):当前显示页 > 新建。
    AgentBrowserPageResolution resolve_agent_page(
        const std::string& requested_page_id,
        const AgentBrowserPageOwner& owner) const;

    // 关闭 `page_id` 之后应显示的页面;它不是显示页时返回当前显示页。优先同会话
    // 最近的一页,其次全局最近的一页,都没有则为空。须在 remove_page 之前调用。
    std::string next_displayed_after_close(const std::string& page_id) const;

private:
    std::string latest_page_for_session(const std::string& session_id,
                                        const std::string& excluding) const;

    std::vector<std::string> order_;
    std::unordered_map<std::string, AgentBrowserPageOwner> owners_;
    std::unordered_map<std::string, std::string> agent_target_by_session_;
    std::string displayed_;
};

} // namespace acecode::desktop
