# Design

## Context

动机见 proposal.md「Why」,行为要求见 `specs/agent-browser-cookie-storage/spec.md`。

**参照实现(Claude 桌面版 2.42304.2,逆向自 `app.asar` 主进程包与 `ion-dist` 界面包):**

- 设置项 `launchPreviewStorage`,取值 `none | shared | session`。默认值写在偏好默认表里,是 `none`。界面文案分别是 Until quit / Shared / Per session。
- 每档对应一个 Electron session 分区,由 `nN()` 生成分区名:
  - `none`:不带 `persist:` 前缀的内存分区 `launch-preview-<md5(cwd) 前 12 位>`。
  - `shared`:`persist:launch-preview-cowork-shared`。
  - `session`:`persist:launch-preview-session-<会话 id>`。
  - 本机磁盘上可以看到 `%APPDATA%\Claude\Partitions\launch-preview-cowork-shared\Network\Cookies`。
- 已经落过盘的分区记在 `launchPreviewPersistedWorkspaces` 和 `launchPreviewSessionScopedSessions` 两个清单里,供清理时枚举。
- 清理函数 `OM()` 依次调用 `session.clearData()` 和 `clearAuthCache()`。
- 切回 `none` 时(包括以 `none` 启动时)调用 `Phi()`,把所有持久分区清空。
- 切换档位时 `khi()` 把已打开的面板按原 URL 重新绑定到新分区。
- 永久删除会话时调用 `Fhi()` 清掉该会话的分区;归档会话不清。
- 分叉会话时 `Ihi()` 用 `cookies.get` + `cookies.set` 把父会话分区的 cookie 复制到子会话分区。
- Electron 的落盘会话写死了 `persist_session_cookies = false`,所以 Claude 也不保存没有过期时间的 cookie。

**ACECode 现状:**

- **Windows**:Desktop 启动时创建唯一的 WebView2 environment,用户数据目录为 `<data_dir>/agent_browser/webview2`(旧装机是 `agent-browser/webview2`)。创建 controller 时不传任何选项,所有页面都落在默认 Profile。见 `agent_browser_host.cpp:968-995` 和 `:1106-1107`。
- **macOS**:使用固定 UUID 的 `dataStoreForIdentifier:`,所有页面共用。见 `agent_browser_host_mac.mm:855-870`、`:1208` 和 `:1304`。
- 两端都没有任何清除代码。
- 本机磁盘上已经存了 115 个站点、334 条带过期时间的 cookie,没有会话 cookie 落盘。也就是说,现状实际上就是 Claude 的 Shared 档。
- 页面归属已有 `session_id` / `root_session_id`(`agent_browser_page_directory.hpp`),子代理页面的 `root_session_id` 指向父会话。
- 会话数据在 daemon 进程,cookie 存储在 Desktop 进程。会话删除已有进程内通知 `add_session_purge_listener`(`session_purge_listeners.hpp`)。分叉的两个入口(Web 路由、线程服务)都走 `SessionManager::fork_session_to_new_id`。
- 仓库钉的 WebView2 SDK 是 1.0.4078.44,头文件里以下接口都有:
  - 创建与选项:`CreateCoreWebView2ControllerOptions`、`put_ProfileName`、`put_IsInPrivateModeEnabled`、`CreateCoreWebView2ControllerWithOptions`
  - Profile 操作:`ICoreWebView2Profile2::ClearBrowsingDataAll`、`ICoreWebView2Profile5::get_CookieManager`、`ICoreWebView2Profile8::Delete`

## Goals / Non-Goals

**Goals:**

- 存储键的计算、切档 / 清除 / 删除 / 分叉复制的判定集中在一个纯逻辑模块里,两端宿主共用,可以单测。
- Shared 档沿用现有存储,升级零迁移、零掉登录。
- 会话删除与分叉不要求 Desktop 当时在运行。

**Non-Goals:**

- 不做从其他浏览器导入 cookie。
- 不改变会话 cookie 的引擎行为。
- 不为 Linux / Deepin 桌面壳实现。那边没有 Agent Browser 原生宿主。
- 不做「按工作区」档。Claude 的 `launch-preview-<cwd 哈希>` 只用于开发服务器预览,ACECode 没有对应的面板。

## Decisions

### D1 存储键:三种键,子代理归根会话

纯逻辑函数 `cookie_store_key(mode, owner)` 只返回三种键:

- `until_quit`
- `shared`
- `session:<root_session_id>`

规则:

- 只有 `per_session` 档才会返回 `session:` 键。
- `root_session_id` 为空时退回到 `session_id`。
- owner 为空(旧 daemon、未绑定页面)时一律返回 `shared`。

为什么子代理归根会话:子代理是在替父会话干活,登录态理应相通。现有 owner 已经带 `root_session_id`,不用新增字段。

**备选**:按 `session_id` 各存一份。子代理每次都要重新登录,不采用。

### D2 Windows:每种键对应一个 WebView2 Profile,共用同一个 environment

| 键 | ProfileName | InPrivate |
|---|---|---|
| `shared` | 不设置(默认 Profile) | 否 |
| `until_quit` | `acecode-until-quit` | 是 |
| `session:<id>` | `s-` + SHA-256(id) 的前 24 个十六进制字符 | 否 |

- 所有 controller 统一改用 `CreateCoreWebView2ControllerWithOptions`。
- Shared 不设 ProfileName,是为了继续用现有的默认 Profile,升级后登录还在。
- 会话 id 要转成哈希,因为 Profile 名有限制:最多 64 个字符,只允许字母、数字和 `#@$()+-_~.` 及空格,而且大小写不敏感。headless 允许的自定义会话 id 也可能超过 64 个字符。
- 同一个 environment 下可以同时存在多个 Profile。这样只需要一个浏览器进程,环境创建与 manifest 发布流程都不用动。

**Until quit 用 InPrivate,外加一个隐藏的保活页面。** 选它,是因为 InPrivate 是 WebView2 里唯一不往磁盘写 cookie 的方式,最接近 Claude 的内存分区。Chromium 的无痕 Profile 在最后一个使用者关闭后就会销毁,而规格要求「关掉所有页面再打开仍保持登录」。所以第一次用到这个键时,宿主额外创建一个不可见的 `about:blank` controller,一直保持到切离该档或 Desktop 退出。tasks 第 2 组第一项会先实测:如果不加保活、关掉所有页面后数据仍在,就去掉保活。

**备选**:用普通 Profile,启动和退出时各清空一次。这样实现最简单,但 cookie 会在运行期间落盘;异常退出时,数据会一直留到下次启动。与 Claude「从不落盘」的语义不一致,不采用。

### D3 拿到 Profile 句柄:借用页面,没有页面时临时建一个隐藏页面

WebView2 只能从某个 webview 上通过 `ICoreWebView2_13::get_Profile` 拿到 Profile。

- 目标键已有页面时,直接借用该页面的 Profile。
- 目标键没有页面时,创建一个不可见的 `about:blank` controller,操作完成后立即关闭。

清除、删除、复制 cookie 三种操作都走同一个 UI 线程上的工具函数 `with_profile(key, callback)`。

### D4 切档与「待清空」标记:不为每个会话存储逐个开隐藏页面

切到 `until_quit` 时的处理:

- `shared` 存储立即执行 `ClearBrowsingDataAll`。它最可能正在用,不能等。
- 每个 `session:` 存储只是在状态文件里记一个「待清空」标记。

按会话存储可能有上百个,逐个开隐藏页面去清会很慢。而且在 `until_quit` 档下这些存储根本不会被用到。待清空的存储有两条清理路径:

- **下次启动**:在创建 environment 之前,直接删除磁盘上的 Profile 目录。这时 Desktop 是单实例,还没有浏览器进程占用这些目录。
- **同一次运行中又切回 `per_session`**:该会话第一次建页面前,先对它执行 `ClearBrowsingDataAll`,再清掉标记。

`shared` 的清空同样靠持久标记保证不丢:切档时先把 `shared_wipe_pending` 写进状态文件,清空成功后才撤掉;清空失败或中途崩溃时标记留着,下次启动在 environment 创建后、第一个页面创建前再执行一次 `ClearBrowsingDataAll`。`until_quit` 档下没有页面使用 `shared` 存储,它不会再被写入,所以只需在标记存在时补清,不必每次以 `until_quit` 启动都清。

切换档位**不**使用 `Profile8::Delete`。被标记删除的 Profile 在同一次运行中不能再用同名重建,而用户可能马上又切回去。`Delete` 只用于永久删除会话,那种会话 id 不会再出现。

### D5 macOS:三种 data store

| 键 | data store |
|---|---|
| `shared` | 现有固定 UUID 的 `dataStoreForIdentifier:` |
| `until_quit` | 宿主持有的单个 `nonPersistentDataStore`,切离该档或退出时释放 |
| `session:<id>` | `dataStoreForIdentifier:`,UUID 由 SHA-256(命名空间前缀 + id) 派生,并设置版本位与变体位 |

各项操作的实现:

- **清空**:`removeDataOfTypes:allWebsiteDataTypes modifiedSince:distantPast`。
- **删除**:先关闭页面、释放 store 对象,再调用 `+removeDataStoreForIdentifier:`。
- **复制 cookie**:`httpCookieStore getAllCookies:` 读出,再逐条 `setCookie:` 写入。

macOS 没有 Windows 那种「必须借用页面才能拿到 Profile」的限制,D3 只适用于 Windows。D4 的「待清空」标记两端共用。macOS 下启动时的处理改为对每个待清空的 identifier 调用 `removeDataStoreForIdentifier:`。Agent Browser 本身只在 macOS 14+ 启用,而这些接口在 14+ 都可用。

### D6 共享状态:`state.json` 的 `agent_browser` 段

由新模块 `src/base/ipc/agent_browser_store_state` 独占读写,沿用 `update_state_json` 的跨进程锁与原子写。

```json
"agent_browser": {
  "cookie_mode": "shared",
  "session_stores": ["<root id>", "..."],
  "pending_wipe": ["<root id>", "..."],
  "pending_deletions": ["<root id>", "..."],
  "pending_seeds": [{"session_id": "<child>", "from": "<parent>", "at_ms": 0}]
}
```

- `cookie_mode`、`session_stores`、`pending_wipe` 只由 Desktop 写入。
- `pending_deletions`、`pending_seeds` 由 daemon / TUI 追加,由 Desktop 消费。

**为什么放在 `state.json`,不放 `config.json`**:这个值和 Claude 一样,属于界面偏好,只由 Desktop 一个进程写;`state.json` 已经有跨进程锁。放进 `config.json` 会和 daemon 的整份配置写回互相覆盖。

`pending_seeds` 最多保留 500 条,超出时丢最旧的。这样在 shared 档下(这些记录永远不会被消费)不会无限增长。

### D7 daemon 侧:删除与分叉两个通知,一个服务

会话层新增 `add_session_fork_listener`,写法与现有 purge 通知对称,在 `fork_session_to_new_id` 成功后触发。

新服务 `host/session_host/agent_browser_store_lifecycle` 在 worker 与 TUI 启动时注册两个监听器:

- **分叉**:向 `pending_seeds` 追加一条记录。
- **永久删除**:向 `pending_deletions` 追加该 id,然后在服务自有的工作线程上向 Desktop 发一次代理操作 `drain_store_tasks`(超时 2 秒)。

这个代理操作不带任何数据,只是提醒 Desktop 去处理状态文件。提醒失败只记一条 debug 日志,包括旧版 Desktop 回复「operation is invalid」的情况。

监听器回调本身只做状态文件写入,不做网络 IO,符合「不应长时间阻塞」的约定。工作线程与回调捕获遵守 R15 所有权规则:使用 `JoiningThread`,`LifetimeToken` 放在最后一个成员。

**为什么不升协议版本**:分叉来源不进 owner,而是通过状态文件交接;提醒失败也无害。所以新旧版本混用时不需要互相拒绝,对应规格最后一条。

**备选**:把 `forked_from` 加进 owner。前端的会话列表没有这个字段,UI 自建页面时拿不到,还要升协议。不采用。

### D8 Desktop 侧:处理时机与顺序

**启动时,创建 environment 之前:**

1. 读取 `agent_browser` 段。
2. 对 `pending_wipe` 和 `pending_deletions` 中的会话,删除磁盘上的 Profile 目录(Windows)。macOS 改为调用 `removeDataStoreForIdentifier:`。
3. 从 `session_stores` 中移除这些会话,清空这两个清单。

**运行中收到 `drain_store_tasks` 时:**

1. 关闭该根会话的全部页面,同时发出页面状态事件,让前端页签消失。
2. 用 D3 的方式调用 `Profile8::Delete`。

**首次为 `session:<id>` 建页面之前:**

1. 如果它在 `pending_wipe` 中,先清空数据。
2. 如果它不在 `session_stores` 中,而 `pending_seeds` 里有它的记录,并且父会话在 `session_stores` 中,就把父会话的 cookie 复制过来(D3/D5)。
3. 记入 `session_stores`,删除对应的 seed 记录。

复制的是父会话此刻的 cookie,不是分叉那一刻的快照。这与 Claude 的即时复制有细微差别,规格里已写明「as they are at that moment」。

### D9 切档时重建页面

纯逻辑函数 `pages_to_rebuild(pages, old_mode, new_mode)` 返回存储键发生变化的页面。宿主对其中每一页:

1. 记下 URL;owner 与共享标记保存在页面对象上,重建时不变(设备模拟尚未实现,没有要恢复的状态)。
2. 关闭旧的 controller。
3. 在新键对应的 Profile 中用同一个 page id 创建 controller,并导航回原 URL。

页面目录(`agent_browser_page_directory`)里的显示页和 Agent 目标页不变,前端看到的是同一个页签重新加载了一次。后退历史会丢失,这一点可以接受,Claude 的重绑也是重新加载。

### D10 前端

**纯逻辑**:`web/src/lib/agentBrowserCookieMode.js`,负责:

- 三档的取值、标签与说明文案;
- 每档下「清除浏览数据」确认框的标题、正文和作用范围;
- 一个单写者 store,供工具栏与设置页共同订阅。

**Bridge**,封装在 `agentBrowser.js`:

| 函数 | 作用 |
|---|---|
| `aceDesktop_agentBrowserGetCookieMode` | 读取当前档位 |
| `aceDesktop_agentBrowserSetCookieMode(mode)` | 设置档位 |
| `aceDesktop_agentBrowserClearBrowsingData(owner)` | 清除浏览数据 |

Desktop 在档位变化后推送状态事件,前端据此更新 store。

**工具栏**:在「开发者工具」按钮后加一个「更多」按钮,菜单结构仿照 Claude 截图:

- 分组标题「Cookie」
- 「保留 Cookie」:右侧显示当前档位,点开是子菜单,含三项,每项附一行说明
- 「清除浏览数据」:危险色

菜单和确认框会盖在原生页面上方,所以必须接入现有的原生表面遮挡机制(`fix-agent-browser-overlay-stacking`),否则会被原生页面挡住。

**确认框**:走 `components/Modal.jsx`,危险操作默认聚焦「清除」。

**设置页**:「设置 > 工具」的 Agent 浏览器条目下方增加同样的三档单选,遵循 `acecode-frontend-style` 规范。

新增的中文文案先补 `i18n-en-overrides.mjs`,再运行 `pnpm i18n:catalog`。

## Risks / Trade-offs

- **[InPrivate Profile 的生命周期]**(2026-10-21 实测,WebView2 SDK 1.0.4078.44 + 本机运行时)
  不加保活页:关掉所有「退出即清」页面后再打开,cookie 已丢失(`agent_browser_cookie_smoke` 在 `until_quit_after_closing_all_pages` 处失败)。加保活页:cookie 保留,Desktop 重启后消失。
  → 保活页面必须保留(D2 的做法成立)。

- **[WebView2 Profile 目录的实际位置]**(2026-10-21 实测)
  命名 Profile 的目录是 `<UDF>/EBWebView/WV2Profile_<ProfileName>`,不是最初假设的 `EBWebView/<ProfileName>`;默认 Profile 是 `EBWebView/Default`。
  → 目录名由 `agent_browser_session_profile_directory_name` 统一派生并有单测;冒烟里核对目录存在与删除。这条布局属于运行时实现细节,若将来变化,启动删除只会落空(数据留在磁盘),冒烟的 `profile_exists` 检查会先报出来。

- **[会话 cookie 在重启后丢失,用户仍然觉得「没记住登录」]**
  2026-10-21 冒烟实测:共享档下没有过期时间的 cookie 在 Desktop 重启后消失(`session_cookie_after_restart false`),带过期时间的保留。
  → 这是引擎行为,Claude 也一样。帮助文档里说明:网站登录时勾选「记住我」,登录态才能跨重启保留。

- **[切档重建页面时丢失页内状态]**
  会丢失表单草稿、后退历史,以及运行中的 Agent 操作。
  → 切档是用户主动操作,重建前不做额外提示;正在执行的浏览器工具调用会因页面重载而失败,模型按页面已重新加载处理重试。

- **[按会话存储数量增长,数据目录迁移变慢]**
  → 删除会话时会同步删除对应存储;迁移本来就把这棵子树当作「尽力复制」处理(迁移代码原先只认连字符旧目录名,已在同期单独修复,下划线新目录同样按尽力复制处理)。

- **[状态文件写入被迁移暂停]**
  `set_state_file_writes_paused` 期间,`pending_deletions` 写不进去。
  → 监听器在写入失败时记一条 warn 日志,该会话的存储会成为孤儿目录,但不影响功能。迁移期间本来就拒绝删除忙碌会话,这种情况很少见。

- **[多个按会话存储同时打开,内存上涨]**
  每个 Profile 会有独立的网络上下文。
  → 只有已经打开过页面的会话才会加载 Profile,与现在每个页面一个渲染进程的开销相比可以忽略。

## Migration Plan

- 升级后没有写入任何档位时按 `shared` 运行,沿用默认 Profile / 固定 UUID store,不需要迁移。
- 回滚到旧版本时,旧版只认默认 Profile,`state.json` 里的 `agent_browser` 段会被忽略。按会话存储会作为孤儿目录留在用户数据目录里,不影响旧版运行。如果用户之前用过 `until_quit`,默认 Profile 已经被清空过,回滚后同样是未登录状态,与预期一致。
