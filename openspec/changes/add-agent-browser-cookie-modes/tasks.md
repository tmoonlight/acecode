# Tasks

## 1. 纯逻辑与共享状态

- [x] 1.1 新增 `src/apps/desktop/agent_browser_cookie_jars.{hpp,cpp}`,实现以下纯逻辑(设计见 D1、D4、D8、D9):
  - 档位解析:未知值按 `shared` 处理;
  - `cookie_store_key(mode, owner)`;
  - Windows Profile 名派生:`s-` + SHA-256 的前 24 位十六进制;
  - macOS 按会话 UUID 派生;
  - 切到某档时哪些存储立即清空、哪些打「待清空」标记;
  - `pages_to_rebuild`;
  - 首次建存储时的清空与复制判定。

  验收:`tests/desktop/agent_browser_cookie_jars_test.cpp` 通过(中文注释写明场景),至少覆盖:子代理归根会话、空 owner 归 shared、超长会话 id 的 Profile 名合法、shared 与 per_session 互切不清数据。
- [x] 1.2 新增 `src/base/ipc/agent_browser_store_state.{hpp,cpp}`,独占读写 `state.json` 的 `agent_browser` 段(D6),包括:读写档位,追加和消费 `pending_deletions` / `pending_seeds` / `pending_wipe`,维护 `session_stores`,`pending_seeds` 上限 500 条。

  验收:`tests/ipc/agent_browser_store_state_test.cpp` 通过,用 `set_state_file_path_for_test` 隔离;覆盖损坏段落容错、上限截断、并发追加不丢条目。

## 2. Windows 宿主

- [x] 2.1 实测并记录 WebView2 行为,结论写回 design 的 Risks:
  - InPrivate 命名 Profile 在关闭所有页面后是否丢失 cookie、加保活页面后是否保留、重启后是否消失;
  - 命名 Profile 在磁盘上的目录位置。

  验收:design.md 中两条风险更新为实测结论,并据此确定保活页面与启动删除方式。
- [x] 2.2 `agent_browser_host.cpp` 的建页改用 `CreateCoreWebView2ControllerWithOptions`,按 `cookie_store_key` 设置 ProfileName / InPrivate;Until quit 档按 2.1 的结论创建保活页面;实现 `with_profile` 工具函数(D3)。

  验收:新增 `agent_browser_cookie_smoke`(独立的 Windows 冒烟,输出 `SMOKE_COOKIE_MODES_OK`),覆盖三档的隔离、子代理归根与 Until quit 关页后保留;共享档沿用无选项建 controller 的原路径(默认 Profile),升级不掉登录的手工确认在 6.2。
- [x] 2.3 实现切档(D4、D9):
  - shared 立即 `ClearBrowsingDataAll`,per_session 存储打待清空标记;
  - 按 `pages_to_rebuild` 用原 page id 重建页面并恢复 URL、owner 与共享标记;
  - 以 until_quit 启动时,在创建 environment 前删除待清空目录,创建后清空 shared。

  验收:smoke 段覆盖「切到 until_quit 后再切回 shared 已无 cookie」和「切档后原 page id 仍可用 CDP 调用」。
- [x] 2.4 实现会话存储的生命周期(D8):
  - 启动时处理 `pending_deletions`;
  - 新代理操作 `drain_store_tasks`:关闭该根会话的页面并发状态事件,然后 `Profile8::Delete`;
  - 首次建会话存储前处理待清空与分叉复制(用 Profile5 CookieManager 逐条读写)。

  验收:冒烟覆盖「运行中删除已加载的会话存储后其页面关闭、重启后目录被删」「未加载的会话存储经代理提醒直接删目录」「分叉复制后两边互不影响」。
- [x] 2.5 `main.cpp` 新增三个 bridge:`aceDesktop_agentBrowserGetCookieMode`、`SetCookieMode`、`ClearBrowsingData(owner)`;档位变化时推送状态事件。

  验收:bridge 只是宿主接口的薄封装,宿主接口由冒烟覆盖;`agentBrowserArchitecture.test.js` 的 bridge 断言随第 5 组前端改动一起补。

## 3. macOS 宿主

- [ ] 3.1 `agent_browser_host_mac.mm` 按 D5 实现三种 data store,以及清空、删除、复制 cookie、切档重建、启动处理,与 Windows 共用第 1 组的纯逻辑和状态模块;同步接上三个 bridge。

  验收:本机无法编译 macOS,用 `ios-simulator-live.yml` 同类的手动 macOS 工作流(或推 `ci/` 分支跑 package.yml 的 macOS 构建)确认能编译通过;行为验收列入第 6 组的手工清单。
- [x] 3.2 更新 `docs/agent-browser-macos-handoff.md` 的 Profile 一节,改为三种 data store 的说明。

  验收:文档描述与 D5 一致。

## 4. daemon / TUI

- [x] 4.1 `src/domain/session/` 新增 `add_session_fork_listener` / `notify_session_forked`,在 `SessionManager::fork_session_to_new_id` 成功后触发。

  验收:`tests/session/` 新增用例,确认 Web 分叉路由与线程服务分叉两个入口都会触发,失败的分叉不触发。
- [x] 4.2 新增 `src/host/session_host/agent_browser_store_lifecycle.{hpp,cpp}`(D7):
  - 监听分叉,写 `pending_seeds`;
  - 监听永久删除,写 `pending_deletions`;
  - 自有 `JoiningThread` 发送 `drain_store_tasks` 提醒,超时 2 秒,失败只记 debug 日志;
  - 在 `worker.cpp` 与 TUI 运行时初始化中各注册一次。

  验收:单测用假代理确认写入内容与提醒发送;旧版 Desktop 回复「operation is invalid」时不报错;R15 所有权检查(`check_ownership --strict --final`)通过。
- [x] 4.3 在 Agent Browser 代理协议的说明里补充 `drain_store_tasks` 操作(代理操作列表写在 `docs/agent-browser.md`「运行时和安全边界」,`daemon-api.md` 没有这一节)。

  验收:文档列出该操作的请求、返回与失败语义。

## 5. Web UI

- [x] 5.1 新增 `web/src/lib/agentBrowserCookieMode.js`(D10):三档取值与文案、每档清除确认的标题/正文/范围、单写者 store,以及 bridge 封装(在 `agentBrowser.js` 中)。

  验收:`agentBrowserCookieMode.test.js` 已注册进 `runTests.js` 并通过,覆盖非 Desktop 环境返回「不可用」、bridge 失败时保持原值。
- [ ] 5.2 `AgentBrowserPanel.jsx` 工具栏新增「更多」菜单:Cookie 分组、「保留 Cookie」子菜单(三项各带说明、当前项打勾)、危险色的「清除浏览数据」与 `Modal` 确认框;菜单与确认框接入原生表面遮挡机制。

  验收:在 Desktop 中打开菜单不被原生页面遮住(截图确认);`agentBrowserArchitecture.test.js` 的 floatingSurfaceOwners 等合同测试通过。
- [x] 5.3 「设置 > Agent 浏览器」新增三档单选,与工具栏共用 store,遵循 `acecode-frontend-style`。

  验收:在任一处改档位,另一处立即同步;设置搜索能用「cookie」「登录」找到该项(`settingsSearch.js` 索引用例)。
- [x] 5.4 新增中文文案,先补 `i18n-en-overrides.mjs` 再运行 `pnpm i18n:catalog`。

  验收:i18n 目录检查测试通过,英文界面显示 Until quit / Shared / Per session 对应文案。

## 6. 集成验证与文档

- [x] 6.1 构建 `acecode-desktop` 与 `acecode_unit_tests`,运行第 1、2、4 组新增的单测与 Windows smoke;运行 `pnpm test` 与 `pnpm build`。

  验收:构建退出码为 0,所有测试通过。
- [ ] 6.2 按 spec 的场景逐条手工验收(Windows 本机):
  - 默认 Shared 升级不掉登录;
  - 三档的重启与跨会话行为;
  - 切到 Until quit 立即清空;
  - 两种范围的清除;
  - 删除会话(Desktop 开着、关着各一次)、归档不清、分叉继承。

  macOS 各项列为待补验。

  验收:结果记录在本变更目录的 `verification.md`,未执行的项标为待补验,不宣称通过。
- [x] 6.3 更新文档:
  - `docs/agent-browser.md`:持久化一节改为三档说明,并修正目录名为 `agent_browser/webview2`(旧装机是 `agent-browser`);
  - `docs/help/agent-browser.html`:用户说明,含「勾选记住我才能跨重启保持登录」;
  - `CLAUDE.md` 的 Agent Browser 段:关键约束,包括 shared 不设 ProfileName、切档不用 `Profile8::Delete`、状态段读写归属。

  验收:文档描述与实现一致。

## Workflow follow-up

- 数据目录迁移只认 `agent-browser/webview2`,不认 `agent_browser/webview2`:已在同期单独修复(`migration_is_best_effort_root` 两种都认,回归 `UnderscoreBrowserProfileChurnDoesNotFailMigration`)。
- 实现、验收完成后归档本变更。
