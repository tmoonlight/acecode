# add-agent-browser-cookie-modes 验收记录

记录日期:2026-10-21。实施状态与验收状态分开记:下面「自动化」是已经实际跑过的结果,
「手工」一栏未执行的项一律标 **待补验**,不宣称通过。

## 1. 自动化结果(Windows 本机,构建目录 `N:/acecode-build/cookie-modes`)

| 项目 | 结果 |
|---|---|
| `acecode-desktop`、`acecode_unit_tests` 构建 | 退出码 0 |
| 单测 `AgentBrowserCookieMode*` / `AgentBrowserCookieStoreKey*` / `AgentBrowserWindowsProfile*` / `AgentBrowserSessionStoreUuid*` / `AgentBrowserCookieModeSwitch*` / `AgentBrowserPagesToRebuild*` / `AgentBrowserSessionStorePreparation*` | 通过 |
| 单测 `AgentBrowserStoreStateTest.*`(10 条,含损坏值、500 条上限、只删本段不动其它键) | 通过 |
| 单测 `SessionForkListener.*`(4 条)、`AgentBrowserStoreLifecycleTest.*`(5 条,含旧 Desktop 不认识操作、连续删除合并提醒、停止时中止) | 通过 |
| 单测数据目录迁移(含新增 `UnderscoreBrowserProfileChurnDoesNotFailMigration`) | 通过 |
| 冒烟 `agent_browser_cookie_smoke`(真实 WebView2 + example.com,三个宿主模拟两次重启) | `SMOKE_COOKIE_MODES_OK` |
| 既有 Agent Browser 宿主冒烟 | 通过 |
| `pnpm test` | 3295 条全部通过(含 `agentBrowserCookieMode.test.js`、`submenuPosition.test.js`、`agentBrowserArchitecture.test.js`、`settingsSearch.test.js`、i18n 目录检查) |
| `pnpm build` | 通过;Desktop 已用新前端重新 configure + 构建 |
| R15 所有权检查(`check_ownership --strict --final`) | 本变更新增代码无发现;仍有一条 `linux_desktop` 的既有发现,HEAD `01d74217` 上已存在,与本变更无关 |
| macOS 编译 | **待补验**:本机无法编译,`agent_browser_host_mac.mm` 改动未编译 |

冒烟的观察项(WebView2 运行时行为,写回 design 的 Risks):

- 不加保活页时,关掉所有「退出时清除」页面后 cookie 丢失 → 保活页必须保留;
- 命名 Profile 目录是 `EBWebView/WV2Profile_<名>`;
- 共享档下无过期时间的会话 cookie 重启后丢失(`session_cookie_after_restart false`),带过期时间的保留。

## 2. 按 spec 场景对照

「自动化」列写覆盖它的测试;「手工」列是在真实 Desktop + daemon 上的界面验收。

| 场景 | 自动化 | 手工(Windows) | macOS |
|---|---|---|---|
| Fresh install(默认 Shared) | 冒烟宿主 1 开头断言;`StoredNamesResolveWithSharedAsDefault` | 待补验 | 待补验 |
| Corrupted stored value | `CorruptedSectionIsTreatedAsEmpty`、`UnknownCookieModeIsKeptVerbatimButBounded`、`StoredNamesResolveWithSharedAsDefault` | — | — |
| Sign-in survives restart(Shared) | 冒烟宿主 2:`smk_persist` 重启后仍在 | 待补验 | 待补验 |
| Upgrade keeps existing sign-ins | 无(Shared 沿用默认 Profile / 固定 UUID,按构造成立) | 待补验:用旧版登录某站,升级后打开仍登录 | 待补验 |
| Two sessions see the same sign-in | 冒烟宿主 2:会话 X 的页面读到会话 A 在共享档写入的 `smk_persist` | 待补验 | 待补验 |
| Until quit:Closing all pages keeps the sign-in | 冒烟 `until_quit_after_closing_all_pages` | 待补验 | 待补验 |
| Until quit:Restart signs out | 冒烟宿主 3:`smk_quit2` 重启后消失 | 待补验 | 待补验 |
| Per session:Sessions do not see each other's sign-in | 冒烟宿主 1:会话 A / B 互不可见 | 待补验 | 待补验 |
| Sub-agent uses its root session's sign-in | 冒烟:root=A 的子代理页读到 `smk_A`;`ModesMapOwnersToStores` | 待补验 | 待补验 |
| Per-session sign-in survives restart | 冒烟宿主 2 前的目录与状态检查(启动只删待清空 / 待删除的) | 待补验 | 待补验 |
| Page without owner | `OwnerlessOrInvalidOwnerFallsBackToShared` | — | — |
| Session-only sign-in cookie | 冒烟观察 `session_cookie_after_restart false`;帮助页已说明「记住我」 | — | — |
| Change from toolbar menu(两处同步) | `agentBrowserCookieMode.test.js`(共用 store) | 待补验:工具栏改档,设置页立即同步,反之亦然 | 待补验 |
| Not running in Desktop | `agentBrowserCookieMode.test.js`(无 bridge 时不可用,菜单与设置项不显示) | 待补验:普通浏览器打开 Web UI 不出现 cookie 项 | — |
| Switch Shared → Per session with open pages | 冒烟:切档后同一 page id 回到原地址;`OnlyPagesWhoseStoreChangesAreRebuilt` | 待补验:页签不消失、重新加载一次 | 待补验 |
| Switch to Until quit(立即清空) | 冒烟:shared 立即清空,A / B 记待清空,宿主 2 启动删目录 | 待补验 | 待补验 |
| Switch between persisted modes(不清数据) | `OnlyUntilQuitDeletesPersistedData` | 待补验 | 待补验 |
| Leftover data after a crash | 冒烟宿主 1→2:切到退出即清后 `shared_wipe_pending` 落盘、清完撤掉;按会话存储的待清空目录在下次启动删除。清空中途崩溃的分支(标记残留→启动补清)无自动化 | 待补验:until_quit 下强杀 Desktop 后重开 | 待补验 |
| Clear in Shared mode | 无独立冒烟(与按会话共用 `clear_browsing_data` 路径) | 待补验:确认框写明影响所有会话 | 待补验 |
| Clear in Per-session mode | 冒烟:清 A 不影响 C | 待补验:确认框写明只影响当前会话 | 待补验 |
| User cancels | 无(确认框文案由 `agentBrowserCookieMode.test.js` 覆盖,取消路径没有) | 待补验:取消后数据仍在 | 待补验 |
| Delete while Desktop runs | 冒烟:运行中删除 C(页面关闭、目录删除)、未加载的 Z 经 `drain_store_tasks` 删除;`PurgeRecordsDeletionAndNotifiesDesktop` | 待补验:在 Web 永久删除会话,Desktop 里它的页签消失 | 待补验 |
| Delete while Desktop is not running | 冒烟宿主 3:上次记下的 C 启动时删目录 | 待补验 | 待补验 |
| Archive keeps the store | 只有永久删除路径会写 `pending_deletions`(lifecycle 只监听 purge) | 待补验:归档后取消归档,登录仍在 | 待补验 |
| Fork then open | 冒烟:C 从 A 播种 `smk_A2`、之后互相独立;`ForkSeedsFromParentOnlyOnce`;`ForkRecordsSeedWithoutNotifying` | 待补验:Web 分叉后在新会话打开浏览器仍登录 | 待补验 |
| Fork already has a store | `SeedSkipsUnusableParentsAndExistingStores` | — | — |
| New Desktop, old daemon | `OwnerlessOrInvalidOwnerFallsBackToShared`(旧 daemon 不带 owner) | 待补验(需要旧版 daemon) | 待补验 |
| New daemon, old Desktop | `DesktopUnavailableIsNotAnError`(旧 Desktop 回复 operation is invalid 只记 debug) | 待补验(需要旧版 Desktop) | 待补验 |

## 3. 界面检查(tasks 5.2,待补验)

- 浏览器工具栏只剩「选择元素」「开发者工具」两个按钮,其右侧是「⋮」;
- 「⋮」菜单含「将控制台日志添加到聊天」、Cookie 分组、「保留 Cookie」子菜单(三项各带说明、当前项打勾)、危险色「清除浏览数据」;
- 菜单、子菜单与确认框都显示在原生网页**上方**,不被遮住(截图确认);
- 确认框打开时默认焦点在「清除」,Esc 取消;
- 英文界面显示 Until quit / Shared / Per session 对应文案。

## 4. 待补验汇总

1. 第 2 节「手工」列标待补验的各项与第 3 节界面检查(Windows 真实 Desktop)。
2. macOS:先在 Mac(或 CI 的 macOS 构建)上编译 `agent_browser_host_mac.mm`,再按第 2 节逐项验收。
