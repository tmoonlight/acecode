#pragma once

#include "agent_browser_page_directory.hpp"
#include "ipc/agent_browser_store_state.hpp"

#include <optional>
#include <string>
#include <vector>

namespace acecode::desktop {

// Agent Browser「保留 Cookie」三档(复刻 Claude 桌面版 launchPreviewStorage):
//   UntilQuit  —— cookie 只活到 Desktop 退出(Claude: none / Until quit)
//   Shared     —— 所有会话共用一份长期存储(Claude: shared),本项目默认
//   PerSession —— 每个根会话一份长期存储,子代理归根会话(Claude: session)
// 这里只有纯逻辑:Windows / macOS 宿主各自把存储键映射到 WebView2 Profile /
// WKWebsiteDataStore。设计见 openspec/changes/add-agent-browser-cookie-modes/design.md。
enum class AgentBrowserCookieMode {
    UntilQuit,
    Shared,
    PerSession,
};

inline constexpr const char kAgentBrowserCookieModeUntilQuit[] = "until_quit";
inline constexpr const char kAgentBrowserCookieModeShared[] = "shared";
inline constexpr const char kAgentBrowserCookieModePerSession[] = "per_session";

const char* agent_browser_cookie_mode_name(AgentBrowserCookieMode mode);
// 只认三个规范名;其它返回空。
std::optional<AgentBrowserCookieMode> agent_browser_cookie_mode_from_name(
    const std::string& name);
// 状态文件里的原文 → 生效档位:空(从未选过)与未知值一律按 Shared。
AgentBrowserCookieMode effective_agent_browser_cookie_mode(const std::string& stored);

enum class AgentBrowserCookieStoreKind {
    Shared,
    UntilQuit,
    Session,
};

struct AgentBrowserCookieStoreKey {
    AgentBrowserCookieStoreKind kind = AgentBrowserCookieStoreKind::Shared;
    std::string session_id;  // 仅 Session 有值:根会话 id

    // 稳定的字符串形态:"shared" / "until_quit" / "session:<id>"。
    std::string id() const;
    bool operator==(const AgentBrowserCookieStoreKey& other) const {
        return kind == other.kind && session_id == other.session_id;
    }
    bool operator!=(const AgentBrowserCookieStoreKey& other) const {
        return !(*this == other);
    }
};

// 页面归属到哪一份存储的会话:根会话优先(子代理归父会话),没有就是自己;
// 空 owner 返回空串。
std::string agent_browser_cookie_store_session(const AgentBrowserPageOwner& owner);

// 当前档位下某个 owner 的页面用哪份存储。只有 PerSession 档才可能返回
// Session 键;owner 为空(旧 daemon、未绑定页面)或会话 id 非法时退回 Shared。
AgentBrowserCookieStoreKey agent_browser_cookie_store_key(
    AgentBrowserCookieMode mode,
    const AgentBrowserPageOwner& owner);

// ---- Windows:存储键 → WebView2 Profile ----
inline constexpr const char kAgentBrowserUntilQuitProfileName[] = "acecode-until-quit";

struct AgentBrowserWindowsProfile {
    std::string name;  // 空 = WebView2 默认 Profile(Shared 沿用升级前的那份)
    bool in_private = false;
};
AgentBrowserWindowsProfile agent_browser_windows_profile(
    const AgentBrowserCookieStoreKey& key);
// "s-" + SHA-256(会话 id) 前 24 个十六进制字符。WebView2 Profile 名最多 64 字符、
// 只允许字母数字与少数符号、大小写不敏感,所以不能直接用会话 id。
std::string agent_browser_session_profile_name(const std::string& session_id);
// 命名 Profile 在磁盘上的目录:<user-data>/EBWebView/WV2Profile_<ProfileName>
// (WebView2 运行时的布局,2026-10-21 冒烟实测;默认 Profile 是 EBWebView/Default)。
std::string agent_browser_session_profile_directory_name(const std::string& session_id);

// ---- macOS:按会话存储的 dataStoreForIdentifier UUID ----
// 由会话 id 确定性派生(SHA-256 前 16 字节,置 RFC 9562 版本 8 与变体位),
// 形如 "XXXXXXXX-XXXX-8XXX-YXXX-XXXXXXXXXXXX"(大写)。
std::string agent_browser_session_store_uuid(const std::string& session_id);

// ---- 切换档位 ----
struct AgentBrowserCookieModeSwitch {
    bool changed = false;
    // 切到 UntilQuit:共享存储立即清空(清空完成前在状态里挂着 shared_wipe_pending)。
    bool clear_shared_now = false;
    // 切到 UntilQuit:所有按会话存储打「待清空」标记,不逐个开页面去清。
    bool mark_session_stores_for_wipe = false;
    // 离开 UntilQuit:释放「退出即清」存储(及其保活页)。
    bool release_until_quit_store = false;
};
AgentBrowserCookieModeSwitch plan_agent_browser_cookie_mode_switch(
    AgentBrowserCookieMode from,
    AgentBrowserCookieMode to);

struct AgentBrowserPageStoreBinding {
    std::string page_id;
    AgentBrowserPageOwner owner;
    AgentBrowserCookieStoreKey store;  // 页面当前实际所在的存储
};
// 切到 new_mode 后存储键会变的页面(按传入顺序)。
std::vector<std::string> agent_browser_pages_to_rebuild(
    const std::vector<AgentBrowserPageStoreBinding>& pages,
    AgentBrowserCookieMode new_mode);
// 归属某个根会话的页面(永久删除会话时关闭它们)。
std::vector<std::string> agent_browser_pages_of_root_session(
    const std::vector<AgentBrowserPageStoreBinding>& pages,
    const std::string& root_session_id);

// ---- 按会话存储第一次被用到时 ----
struct AgentBrowserSessionStorePreparation {
    bool wipe_first = false;     // 在待清空清单里:先清空再用
    std::string seed_from;       // 非空:从这个父会话的存储复制 cookie
    bool register_store = false; // 还不在 session_stores 里:用过之后登记
};
AgentBrowserSessionStorePreparation prepare_agent_browser_session_store(
    const std::string& session_id,
    const AgentBrowserStoreState& state);
// 把一次准备的结果写回状态:登记存储、移出待清空、消费分叉记录。
void apply_agent_browser_session_store_preparation(
    const std::string& session_id,
    AgentBrowserStoreState& state);

} // namespace acecode::desktop
