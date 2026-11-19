#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace acecode::desktop {

// 主窗口 WebView「页面还活着吗」的判定(纯逻辑,无平台依赖)。
//
// 背景:用户反馈机器长时间不关机,解锁屏幕后整个窗口只剩底色,只能重启程序。
// 这个现象至少有四种成因:浏览器主进程退出、渲染进程卡死、页面脚本把界面卸掉、
// 进程都活着但不出画面。单凭「白屏」分不出是哪一种,而窗口一旦变白,宿主此前既不
// 自愈也不留记录。现在宿主在解锁、远程重连、休眠唤醒后往页面里执行一段探针脚本,
// 隔两秒再执行一次,据此给出结论并写进日志:能恢复的自动恢复,不能的也留下一行
// 可以直接定因的证据。平台接线见 web_host_health_win.hpp。

// 探针脚本,由 ICoreWebView2::ExecuteScript 执行,不依赖页面里的任何应用代码,
// 所以页面脚本坏掉、甚至停在导航失败的错误页时也能回报。每次执行回报当前计数并
// 预约一帧;两次采样之间计数没涨,说明这段时间页面一帧都没画。只预约一帧而不是
// 持续循环:常驻的 requestAnimationFrame 循环会让空闲页面一直产生帧。
inline constexpr const char* kPageProbeScript = R"JS((function () {
  var state = window.__aceHostProbe;
  if (!state) state = window.__aceHostProbe = { frames: 0, pending: false };
  var reply = {
    v: 1,
    location: location.origin + location.pathname,
    ready: document.readyState,
    visibility: document.visibilityState,
    focus: document.hasFocus(),
    root: -1,
    frames: state.frames
  };
  var root = document.getElementById('root');
  if (root) reply.root = root.childElementCount;
  if (!state.pending) {
    state.pending = true;
    requestAnimationFrame(function () { state.pending = false; state.frames += 1; });
  }
  return reply;
})())JS";

struct PageProbeSample {
    bool parsed = false;         // 回报是否是探针脚本的格式
    std::string location;        // origin + pathname,不含查询串
    std::string ready_state;     // loading / interactive / complete
    std::string visibility;      // visible / hidden
    bool has_focus = false;
    int root_children = -1;      // #root 的子元素个数;-1 = 文档里没有 #root
    std::int64_t frames = 0;     // 探针装上以来画过的帧数
};

// 解析 ExecuteScript 的回报(JSON 文本)。脚本抛错时回报是 "null",parsed 为 false。
PageProbeSample parse_page_probe_reply(const std::string& result_json);

enum class PageHealthVerdict {
    Healthy,                // 页面在跑,也在出画面
    NotDrawing,             // 脚本在跑,但窗口可见时两次采样之间一帧都没画
    BlankPage,              // 应用页面已加载完,#root 却一直是空的
    NotAppPage,             // 当前文档不是应用页面(例如导航失败的错误页)
    NoReply,                // 探针脚本没有回音:渲染进程卡死或已经不在
    ControllerUnavailable,  // 调用本身失败:WebView 控件已失效
};

struct HostWindowState {
    bool visible = false;
    bool minimized = false;
};

// 两次采样都拿到回报时的结论。窗口隐藏或最小化时页面不出帧是正常的,不算 NotDrawing。
PageHealthVerdict judge_page_health(const PageProbeSample& first,
                                    const PageProbeSample& second,
                                    const HostWindowState& window);

enum class PageHealthAction {
    None,             // 不动,只留日志
    ReloadPage,       // 重新加载页面
    RestartRenderer,  // 结束渲染进程,由「渲染进程退出」那条路径重新加载
    WebviewLost,      // 控件已失效,走「浏览器主进程退出」的恢复
};

// 没回音时不能直接重新加载:卡死的渲染进程不响应导航(在真实 WebView2 上实测,
// 页面脚本死循环时 Reload 无效),要先结束它。「不出画面」和「不是应用页面」只留
// 证据不处置:前者重载救不了合成器的问题,后者 WebView 自带的错误页上本来就有
// 刷新入口。
PageHealthAction action_for_page_health(PageHealthVerdict verdict);

const char* page_health_verdict_name(PageHealthVerdict verdict);

// 日志用的一行摘要,例如
// "visibility=hidden frames=12->12 root=1 ready=complete focus=0 location=http://127.0.0.1:1/"。
std::string describe_page_probe(const PageProbeSample& first, const PageProbeSample& second);

// 触发探活的宿主事件。
enum class HostWakeEvent {
    SessionUnlock,
    RemoteConnect,
    ConsoleConnect,
    PowerResume,
    RendererUnresponsive,
};

const char* host_wake_event_name(HostWakeEvent event);

// WM_WTSSESSION_CHANGE 的 wParam 归类。用户常在远程桌面里使用,「解锁」实际可能是
// 重新连上远程会话,所以连接类事件同样触发探活。
struct SessionChange {
    const char* name = "other";
    bool known = false;
    bool locks = false;        // 之后会话处于锁定
    bool unlocks = false;      // 之后会话处于解锁
    bool disconnects = false;  // 之后会话没有接在任何屏幕上(远程断开 / 切换用户)
    bool connects = false;     // 之后会话重新接上了屏幕
    bool wakes = false;        // 值得探一次页面
    HostWakeEvent wake = HostWakeEvent::SessionUnlock;
};
SessionChange classify_session_change(std::uint32_t wts_code);

// 页面加载完成后要过这么久才允许探活:启动阶段 #root 本来就是空的,不能当成白屏。
inline constexpr std::int64_t kMinimumProbedPageAgeMs = 30 * 1000;

bool page_probe_allowed(bool page_loaded, std::int64_t page_age_ms);

// 主 WebView 失效(浏览器主进程退出)后的处置。重启程序会连带结束各工作区的
// daemon,正在跑的回合随之中断,所以只在用户确实回到这个窗口时才重启:
// 无人值守时保持原状,让后台任务先跑完。系统正在关机 / 注销时浏览器进程被结束
// 是正常现象,一律不处理。
struct WebviewLossContext {
    bool session_ending = false;
    bool session_locked = false;        // 锁屏,或会话没有接在任何屏幕上
    bool window_visible = false;
    bool window_minimized = false;
    bool window_foreground = false;
};

enum class WebviewLossAction {
    Ignore,   // 会话正在结束
    Defer,    // 记下来,等用户回到窗口
    Recover,  // 现在就恢复
};

WebviewLossAction decide_webview_loss(const WebviewLossContext& context);

// 自动重启的跨进程限次。浏览器进程若一启动就退出,无条件重启会变成死循环,
// 所以把最近几次自动重启的时间记在 state.json 里,时间窗内超过上限就不再重启。
struct RecoveryRestartLimits {
    int max_restarts = 2;
    std::int64_t window_ms = 10 * 60 * 1000;
};

// history:此前自动重启的时间(Unix 毫秒)。允许重启时把 now_ms 记进去并返回 true;
// 过期项总会被剔除。时钟回拨产生的「未来」时间按仍在时间窗内处理。
bool admit_recovery_restart(std::vector<std::int64_t>& history,
                            std::int64_t now_ms,
                            RecoveryRestartLimits limits = {});

} // namespace acecode::desktop
