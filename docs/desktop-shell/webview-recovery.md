# 桌面版界面变白后的自动恢复

用户反馈:机器长时间不关机,解锁屏幕后整个窗口只剩底色,只能重启程序。这个现象至少有四种成因,单凭「白屏」分不出是哪一种:

| 成因 | 窗口里实际是什么 |
|---|---|
| 页面里某处渲染报错,整棵界面被卸掉 | 页面还在,`#root` 是空的 |
| 渲染进程卡死 | 画面停在最后一帧或一片底色,点了没反应 |
| 浏览器主进程退出 | WebView 控件整个失效,只剩宿主窗口的底色 |
| 进程都活着但不出画面 | 页面脚本照常运行,就是没有新的帧 |

此前窗口一旦变白,既不会自己恢复,也没有手动恢复的入口(F5 被页面自己拦掉,托盘菜单的内容又是页面推送的),日志里通常也没有留下可以定因的记录。现在分四层处理:能恢复的自动恢复,不能恢复的也留下一行证据。

## 一、页面自己兜底(所有平台)

入口最外层是一个错误边界(`web/src/components/RootErrorBoundary.jsx`)。任何没被接住的渲染错误不再让窗口变白,而是显示一句话加一个「重新加载」按钮,同时把错误和组件栈写进控制台(桌面版会转写进 desktop 日志,行首是 `[web] [root-error-boundary]`)。

按需加载的代码块取不到时(页面活过一次前端重建或 daemon 升级后,旧文件名已经不存在)自动重新加载一次;60 秒内再次失败就停在兜底界面,不会无限刷新。判定逻辑在 `web/src/lib/rootErrorRecovery.js`。

兜底界面只用原生标签和内联样式,不依赖主题、懒加载块或其它组件。

## 二、宿主探活(Windows)

宿主在下面几个时刻往页面里执行一段探针脚本,隔两秒再执行一次:

- 会话解锁、远程桌面重新连上、控制台会话接回;
- 休眠唤醒;
- WebView2 报告渲染进程无响应(约每 15 秒重复上报一次)。

探针不依赖页面里的应用代码,回报三样东西:`document.visibilityState`、两次采样之间有没有画出新的帧、`#root` 的子节点数。结论和处置:

| 结论 | 含义 | 处置 |
|---|---|---|
| `healthy` | 页面在跑,也在出画面 | 无 |
| `no-reply` | 25 秒没有回音,渲染进程卡死 | 结束渲染进程,随后自动重新加载 |
| `blank-page` | 页面加载完了,`#root` 一直是空的 | 重新加载页面 |
| `not-drawing` | 脚本在跑,窗口可见,但一帧都没画 | 只记日志 |
| `not-app-page` | 当前不是应用页面(导航失败的错误页等) | 只记日志 |
| `controller-unavailable` | 调用本身失败,控件已失效 | 走第四节 |

卡死的渲染进程不响应重新加载(在真实 WebView2 上实测:页面脚本死循环时 `Reload` 无效),所以 `no-reply` 的处置是先结束渲染进程;WebView2 随后上报「渲染进程退出」,走原有的自动重新加载。

所有自动重载共用一份限次:10 分钟内最多 3 次,超过后停在 WebView2 自带的错误页(上面有刷新按钮)。页面加载完成不满 30 秒时不探活,避免把正常的启动过程当成白屏。

`not-drawing` 只留证据不处置:重新加载救不了「进程都活着但不出画面」,而它的真正成因目前没有样本可以验证。日志里出现这个结论时,把那一行和当时窗口的照片一起带回来。

## 三、托盘菜单「重新加载界面」(Windows)

托盘菜单里「打开 ACECode」下面多了一项「重新加载界面」。它是固定项,不依赖页面,也不受自动重载的限次约束:

- 控件还在:结束渲染进程,页面随即重新加载。用户点它说明页面已经不对劲,而卡死的页面不响应普通的重新加载,所以不先试 `Reload`。
- 控件已经失效:直接重启桌面壳(见下一节)。

## 四、浏览器主进程退出(Windows)

WebView2 的浏览器主进程退出后,控件无法就地修复,只能重启桌面壳。

重启时不停止各工作区的 daemon:旧进程把它们释放出来,拉起新进程,等新进程发布自己的属主记录后再退出(最多等 20 秒);新进程按「同版本、同安装」的规则接管这些 daemon,并回到上次的会话。这样设计是为了不打断正在跑的回合。daemon 发现属主进程消失后只等不到一秒,等不到新属主、用户又没开「退出后继续运行」就会自行停止,所以旧进程必须等交接完成。

什么时候重启:

- 系统正在关机或注销:不处理。这时浏览器进程先被系统结束是正常现象。
- 屏幕锁着、远程桌面断开着、窗口藏在托盘、最小化,或用户正在用别的程序:先记下来,等用户回到这个窗口再重启(这期间每 2 秒看一次)。
- 用户正在这个窗口前:等 5 秒宽限期后重启。
- 用户点了托盘的「重新加载界面」:立刻重启。

自动重启有跨进程的限次,记在 `state.json` 的 `desktop_webview_recovery_restarts` 里:10 分钟内最多 2 次。浏览器进程若一启动就退出,第 3 次不再自动重启,窗口保持原状,日志里有 `webview-recovery restart skipped`;托盘入口不受这个限制。

## 凭日志定因

日志在 `~/.acecode/logs/desktop_<日期>.log`。下面这些行都以 `[desktop]` 开头:

| 日志里有 | 说明 |
|---|---|
| `session event: unlock` / `remote-connect` / `lock` 等 | 会话状态变化,每次一行 |
| `power event: suspend` / `resume` | 休眠与唤醒 |
| `page probe after <事件>: <结论> (visibility=… frames=a->b root=n …)` | 一轮探活的结论与证据 |
| `page probe skipped after <事件>: no settled page yet` | 页面没加载完或刚加载,没有探 |
| `reloading the page: <原因>` | 宿主发起了一次重新加载 |
| `ended N unresponsive render process(es); …` | 页面没有回音,宿主结束了卡死的渲染进程 |
| `user asked to reload the page: ended N render process(es)` | 用户点了托盘的「重新加载界面」 |
| `page needs a reload (…) but the automatic reload limit is reached` | 重载次数用完 |
| `page probe after <事件> abandoned: script failed …` | 探活途中页面换了文档,本轮作废,不处置 |
| `main page navigation did not complete (web_error_status=…)` | 一次顶层跳转没完成:可能是 daemon 连不上,也可能只是被取消的外链或下载 |
| `WebView render process exited (…); reloading the page` | 渲染进程退出(多为内存耗尽) |
| `WebView render process unresponsive (…)` | 渲染进程无响应 |
| `WebView browser process exited (… shutting_down=0/1)` | 浏览器主进程退出;`shutting_down=1` 是关机 / 注销 |
| `WebView is gone; recovery waits for the user (…)` | 控件已失效,等用户回到窗口 |
| `webview-recovery restart requested (automatic / user request)` | 开始恢复性重启 |
| `[web] [root-error-boundary] …` | 页面渲染报错,带组件栈 |

如果白屏那个时刻前后只有 `page probe after …: healthy`,说明页面脚本和出帧都正常,问题在更外层(显卡、合成器),需要窗口本身的照片进一步判断。

## 代码位置

- 判定逻辑(纯函数):`src/apps/desktop/webview_health.hpp`、`src/apps/desktop/renderer_recovery.hpp`
- Windows 接线(会话 / 电源消息、探活时序、进程故障):`src/apps/desktop/web_host_health.hpp`、`src/apps/desktop/web_host_browser_win.hpp`
- 重启原因、退出收尾、跨进程限次:`src/apps/desktop/desktop_restart.cpp`
- 托盘条目:`src/apps/desktop/tray_menu_layout.hpp`

## 验证

- 单测:`tests/desktop/webview_health_test.cpp`、`tests/desktop/renderer_recovery_test.cpp`、`tests/desktop/desktop_restart_test.cpp`,前端 `web/src/lib/rootErrorRecovery.test.js`。
- 真实 WebView2 冒烟(需手动运行,不在 CI 里):先构建 `webview_health_smoke` 目标,再运行 `python tests/desktop/webview_health_smoke.py <webview_health_smoke.exe>`。五个场景:正常页面探活、一次被取消的跳转之后照样探活、页面脚本死循环后自动恢复、页面脚本死循环后由用户要求重新加载、结束浏览器主进程后的恢复回调。

## 没有在真实桌面版上跑过的

下面三件事只有单测和冒烟覆盖到一部分,整条链路没有端到端跑过:

- 用户在窗口前时的自动重启(冒烟里窗口从不在前台,只走到「等用户回来」和「用户主动要求」两个分支);
- 重启后新进程接管 daemon(单测只守住「释放而不是停止」和「等新属主」两步);
- macOS 与 Linux 上的编译(托盘布局和 `web_host.cpp` 各有几行跨平台改动)。

手动验证恢复性重启:启动桌面版并让它在前台,在 PowerShell 里结束它的主浏览器进程:

```powershell
Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" | Where-Object { $_.CommandLine -notmatch '--type=' -and $_.CommandLine -match 'acecode-desktop\.exe\\EBWebView' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

预期:约 5 秒后桌面版自己重启并回到原来的会话;日志里依次出现 `WebView browser process exited (… shutting_down=0)`、`webview-recovery restart requested (automatic)`、`replacement took over the managed daemons`。先切到别的窗口再执行,则应该等到切回 ACECode 时才重启。

## 没有覆盖的

- macOS 与 Linux 只有第一节(页面兜底)。
- WebView2 不可用时的兼容模式(Edge 应用窗口)下,宿主拿不到这些事件,同样只有第一节。
- 「进程都活着但不出画面」只有证据,没有处置。
- 页面停在脚本的原生对话框(`alert` / `confirm`,HTML 预览的框允许弹)或开发者工具的断点上时,探针同样没有回音。解锁时恰好处于这种状态,约 28 秒后会被当成卡死处理(结束渲染进程并重新加载)。
- 结束渲染进程按 WebView2 环境(用户数据目录)列进程。同时开着的另一个桌面壳实例(开发时多开,或开发版与安装版并存)共用同一个环境,它的页面会跟着重新加载一次。
