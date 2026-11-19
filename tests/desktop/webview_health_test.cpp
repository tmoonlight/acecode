// webview_health 纯逻辑单测。
//
// 背景:用户反馈机器长时间不关机,解锁屏幕后整个窗口只剩底色,只能重启程序。
// 宿主现在在解锁、远程重连、休眠唤醒后往页面里执行探针脚本(隔两秒采两次),
// 按结论自动恢复或留下证据。这里守住「两次采样怎么下结论」「哪种结论才动手」
// 「浏览器主进程退出后什么时候重启」「自动重启的限次」四件事。

#include "desktop/webview_health.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using acecode::desktop::HostWakeEvent;
using acecode::desktop::HostWindowState;
using acecode::desktop::PageHealthAction;
using acecode::desktop::PageHealthVerdict;
using acecode::desktop::PageProbeSample;
using acecode::desktop::RecoveryRestartLimits;
using acecode::desktop::WebviewLossAction;
using acecode::desktop::WebviewLossContext;
using acecode::desktop::action_for_page_health;
using acecode::desktop::admit_recovery_restart;
using acecode::desktop::classify_session_change;
using acecode::desktop::decide_webview_loss;
using acecode::desktop::describe_page_probe;
using acecode::desktop::judge_page_health;
using acecode::desktop::kMinimumProbedPageAgeMs;
using acecode::desktop::page_probe_allowed;
using acecode::desktop::parse_page_probe_reply;

constexpr std::int64_t kMinute = 60 * 1000;
const HostWindowState kVisibleWindow{/*visible=*/true, /*minimized=*/false};

PageProbeSample sample(int root_children, std::int64_t frames,
                       const std::string& visibility = "visible",
                       const std::string& ready_state = "complete") {
    PageProbeSample result;
    result.parsed = true;
    result.location = "http://127.0.0.1:1/";
    result.ready_state = ready_state;
    result.visibility = visibility;
    result.root_children = root_children;
    result.frames = frames;
    return result;
}

// 触发场景:探针脚本在正常的应用页面里执行,ExecuteScript 把返回的对象序列化成 JSON。
// 期望行为:各字段原样解析出来。
TEST(WebviewHealth, ParsesProbeReply) {
    const auto parsed = parse_page_probe_reply(
        R"({"v":1,"location":"http://127.0.0.1:59543/","ready":"complete",)"
        R"("visibility":"hidden","focus":true,"root":1,"frames":42})");
    EXPECT_TRUE(parsed.parsed);
    EXPECT_EQ(parsed.location, "http://127.0.0.1:59543/");
    EXPECT_EQ(parsed.ready_state, "complete");
    EXPECT_EQ(parsed.visibility, "hidden");
    EXPECT_TRUE(parsed.has_focus);
    EXPECT_EQ(parsed.root_children, 1);
    EXPECT_EQ(parsed.frames, 42);
}

// 触发场景:脚本在页面里抛错(回报是 null)、回报被截断、或者页面上别的脚本占用了
// 同名变量导致格式不对。
// 期望行为:parsed 为 false 且不抛异常;字段类型不对时落回默认值。
TEST(WebviewHealth, UnrecognizedProbeReplyIsNotParsed) {
    EXPECT_FALSE(parse_page_probe_reply("null").parsed);
    EXPECT_FALSE(parse_page_probe_reply("").parsed);
    EXPECT_FALSE(parse_page_probe_reply("{\"v\":1,").parsed);
    EXPECT_FALSE(parse_page_probe_reply(R"({"v":2,"root":1})").parsed);
    EXPECT_FALSE(parse_page_probe_reply(R"("a string")").parsed);

    const auto loose = parse_page_probe_reply(R"({"v":1,"root":"x","frames":null,"focus":1})");
    EXPECT_TRUE(loose.parsed);
    EXPECT_EQ(loose.root_children, -1);
    EXPECT_EQ(loose.frames, 0);
    EXPECT_FALSE(loose.has_focus);
}

// 触发场景:解锁后页面一切正常,两次采样之间画了新的帧。
// 期望行为:结论是 Healthy,不做任何处置。
TEST(WebviewHealth, DrawingPageIsHealthy) {
    const auto verdict = judge_page_health(sample(1, 10), sample(1, 11), kVisibleWindow);
    EXPECT_EQ(verdict, PageHealthVerdict::Healthy);
    EXPECT_EQ(action_for_page_health(verdict), PageHealthAction::None);
}

// 触发场景:解锁后窗口可见,页面脚本还在跑,但两秒里一帧都没画(用户看到的就是
// 一片底色)。
// 期望行为:结论是 NotDrawing;只留证据不重载,重载救不了合成器的问题。
TEST(WebviewHealth, VisibleWindowWithoutFramesIsNotDrawing) {
    const auto verdict =
        judge_page_health(sample(1, 10, "hidden"), sample(1, 10, "hidden"), kVisibleWindow);
    EXPECT_EQ(verdict, PageHealthVerdict::NotDrawing);
    EXPECT_EQ(action_for_page_health(verdict), PageHealthAction::None);
}

// 触发场景:窗口被隐藏到托盘或最小化时解锁,页面本来就不出帧。
// 期望行为:不算 NotDrawing,否则每次解锁都会留下一条误报。
TEST(WebviewHealth, HiddenOrMinimizedWindowDoesNotNeedFrames) {
    EXPECT_EQ(judge_page_health(sample(1, 10), sample(1, 10),
                                HostWindowState{/*visible=*/false, /*minimized=*/false}),
              PageHealthVerdict::Healthy);
    EXPECT_EQ(judge_page_health(sample(1, 10), sample(1, 10),
                                HostWindowState{/*visible=*/true, /*minimized=*/true}),
              PageHealthVerdict::Healthy);
}

// 触发场景:应用页面加载完了,#root 却一直是空的(入口脚本没跑起来,或界面被卸掉)。
// 期望行为:结论是 BlankPage,重新加载页面。
// 回归:此前这种状态没有任何恢复途径,窗口只剩底色。
TEST(WebviewHealth, LoadedPageWithEmptyRootIsBlank) {
    const auto verdict = judge_page_health(sample(0, 3), sample(0, 4), kVisibleWindow);
    EXPECT_EQ(verdict, PageHealthVerdict::BlankPage);
    EXPECT_EQ(action_for_page_health(verdict), PageHealthAction::ReloadPage);
}

// 触发场景:页面还在加载,或第一次采样时为空、第二次已经渲染出来。
// 期望行为:不判成空白页,避免把正常的启动过程重载掉。
TEST(WebviewHealth, PageStillRenderingIsNotBlank) {
    EXPECT_EQ(judge_page_health(sample(0, 3, "visible", "interactive"),
                                sample(0, 4, "visible", "interactive"), kVisibleWindow),
              PageHealthVerdict::Healthy);
    EXPECT_EQ(judge_page_health(sample(0, 3), sample(1, 4), kVisibleWindow),
              PageHealthVerdict::Healthy);
}

// 触发场景:WebView 停在导航失败的错误页(daemon 当时连不上),文档里没有 #root;
// 或回报根本不是探针的格式。
// 期望行为:结论是 NotAppPage,只留证据,错误页上自带刷新入口。
TEST(WebviewHealth, DocumentWithoutRootIsNotTheApp) {
    const auto verdict = judge_page_health(sample(-1, 1), sample(-1, 2), kVisibleWindow);
    EXPECT_EQ(verdict, PageHealthVerdict::NotAppPage);
    EXPECT_EQ(action_for_page_health(verdict), PageHealthAction::None);
    EXPECT_EQ(judge_page_health(PageProbeSample{}, sample(1, 2), kVisibleWindow),
              PageHealthVerdict::NotAppPage);
}

// 触发场景:探针脚本迟迟没有回音(渲染进程卡死),或者调用本身就失败(控件已失效)。
// 期望行为:没回音时结束渲染进程,而不是直接重新加载;调用失败转入「控件已失效」
// 的恢复。
// 回归:在真实 WebView2 上实测,页面脚本死循环时 Reload 不起作用,页面一直回不来
// (tests/desktop/webview_health_smoke.py 的 hung 场景)。
TEST(WebviewHealth, SilentPageRestartsRendererAndDeadControlRecovers) {
    EXPECT_EQ(action_for_page_health(PageHealthVerdict::NoReply),
              PageHealthAction::RestartRenderer);
    EXPECT_EQ(action_for_page_health(PageHealthVerdict::ControllerUnavailable),
              PageHealthAction::WebviewLost);
}

// 触发场景:把一次探活写进日志。
// 期望行为:一行里能看到可见性、两次的帧数、#root 子节点数,足以事后定因。
TEST(WebviewHealth, ProbeSummaryCarriesTheEvidence) {
    const auto text = describe_page_probe(sample(1, 12, "hidden"), sample(1, 12, "hidden"));
    EXPECT_NE(text.find("visibility=hidden"), std::string::npos);
    EXPECT_NE(text.find("frames=12->12"), std::string::npos);
    EXPECT_NE(text.find("root=1"), std::string::npos);
    EXPECT_EQ(describe_page_probe(sample(1, 1), PageProbeSample{}), "reply=unrecognized");
}

// 触发场景:WM_WTSSESSION_CHANGE 的各种 wParam。用户常在远程桌面里使用,
// 「解锁」实际可能是重新连上远程会话。
// 期望行为:解锁、远程连接、控制台连接都触发探活;锁定 / 解锁更新锁定状态;
// 断开与未知代码只记录不探。
TEST(WebviewHealth, SessionChangesThatWakeTheProbe) {
    const auto unlock = classify_session_change(0x8);
    EXPECT_TRUE(unlock.wakes);
    EXPECT_TRUE(unlock.unlocks);
    EXPECT_EQ(unlock.wake, HostWakeEvent::SessionUnlock);

    const auto lock = classify_session_change(0x7);
    EXPECT_TRUE(lock.locks);
    EXPECT_FALSE(lock.wakes);

    const auto remote = classify_session_change(0x3);
    EXPECT_TRUE(remote.wakes);
    EXPECT_FALSE(remote.unlocks);
    EXPECT_EQ(remote.wake, HostWakeEvent::RemoteConnect);
    EXPECT_EQ(classify_session_change(0x1).wake, HostWakeEvent::ConsoleConnect);

    // 远程桌面断开后会话不一定上锁,但没有人在看:按「用户不在」记下,重连时解除。
    const auto disconnected = classify_session_change(0x4);
    EXPECT_FALSE(disconnected.wakes);
    EXPECT_TRUE(disconnected.disconnects);
    EXPECT_TRUE(classify_session_change(0x2).disconnects);
    EXPECT_TRUE(remote.connects);
    EXPECT_TRUE(classify_session_change(0x1).connects);
    EXPECT_FALSE(unlock.connects);
    const auto unknown = classify_session_change(0x99);
    EXPECT_FALSE(unknown.known);
    EXPECT_FALSE(unknown.wakes);
}

// 触发场景:程序刚启动或页面刚重载完就遇到解锁事件。
// 期望行为:页面加载满 30 秒才探。启动阶段 #root 本来就是空的,过早探活会把正常
// 启动误判成白屏;重载之后的 30 秒静默也避免连环重载。
TEST(WebviewHealth, ProbeWaitsForASettledPage) {
    EXPECT_FALSE(page_probe_allowed(/*page_loaded=*/false, 10 * kMinute));
    EXPECT_FALSE(page_probe_allowed(/*page_loaded=*/true, kMinimumProbedPageAgeMs - 1));
    EXPECT_TRUE(page_probe_allowed(/*page_loaded=*/true, kMinimumProbedPageAgeMs));
}

// 触发场景:关机 / 注销时系统先结束了浏览器进程(本机日志里 4 次「浏览器主进程
// 退出」全是这种情况)。
// 期望行为:不处理。此时重启程序毫无意义,还会拖慢关机。
TEST(WebviewHealth, BrowserExitDuringSessionEndIsIgnored) {
    WebviewLossContext context;
    context.session_ending = true;
    context.window_visible = true;
    context.window_foreground = true;
    EXPECT_EQ(decide_webview_loss(context), WebviewLossAction::Ignore);
}

// 触发场景:浏览器主进程在无人值守时退出——屏幕锁着、窗口藏在托盘、最小化,
// 或用户正在用别的程序。
// 期望行为:先记下来不动。重启会打扰用户,等他回到这个窗口再做。
TEST(WebviewHealth, BrowserExitWaitsUntilTheUserIsAtTheWindow) {
    WebviewLossContext at_window;
    at_window.window_visible = true;
    at_window.window_foreground = true;
    EXPECT_EQ(decide_webview_loss(at_window), WebviewLossAction::Recover);

    WebviewLossContext locked = at_window;
    locked.session_locked = true;
    EXPECT_EQ(decide_webview_loss(locked), WebviewLossAction::Defer);

    WebviewLossContext hidden = at_window;
    hidden.window_visible = false;
    EXPECT_EQ(decide_webview_loss(hidden), WebviewLossAction::Defer);

    WebviewLossContext minimized = at_window;
    minimized.window_minimized = true;
    EXPECT_EQ(decide_webview_loss(minimized), WebviewLossAction::Defer);

    WebviewLossContext background = at_window;
    background.window_foreground = false;
    EXPECT_EQ(decide_webview_loss(background), WebviewLossAction::Defer);
}

// 触发场景:浏览器进程一启动就退出(运行时损坏等),每次自动重启后马上又失效。
// 期望行为:10 分钟内最多自动重启 2 次,第 3 次拒绝;过了时间窗重新放行。
// 阈值说明:2 次足够越过偶发故障,又不会让一台坏掉的机器陷入无限重启。
TEST(WebviewHealth, AutomaticRestartsAreLimitedAcrossProcesses) {
    std::vector<std::int64_t> history;
    EXPECT_TRUE(admit_recovery_restart(history, 0));
    EXPECT_TRUE(admit_recovery_restart(history, 1 * kMinute));
    EXPECT_FALSE(admit_recovery_restart(history, 2 * kMinute));
    EXPECT_EQ(history.size(), 2u);  // 被拒绝的那次不计入

    EXPECT_TRUE(admit_recovery_restart(history, 10 * kMinute));  // 第一次已过期
    EXPECT_EQ(history.size(), 2u);
}

// 触发场景:系统时钟被往回调,state.json 里留着「未来」的重启时间。
// 期望行为:未来的记录仍算在时间窗内(宁可少重启),不会因为差值为负被当成过期。
TEST(WebviewHealth, ClockRollbackKeepsTheLimit) {
    std::vector<std::int64_t> history{100 * kMinute, 101 * kMinute};
    EXPECT_FALSE(admit_recovery_restart(history, 5 * kMinute));
    EXPECT_EQ(history.size(), 2u);

    RecoveryRestartLimits single;
    single.max_restarts = 1;
    std::vector<std::int64_t> one;
    EXPECT_TRUE(admit_recovery_restart(one, 0, single));
    EXPECT_FALSE(admit_recovery_restart(one, 1, single));
}

} // namespace
