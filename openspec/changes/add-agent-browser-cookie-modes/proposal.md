# Proposal

## Why

Agent Browser 现在只有一份全局共享、长期保存的 Profile:带过期时间的 cookie 会一直留着,但用户无法选择「退出就清」或「会话之间互不可见」,也没有任何清除入口,只能打开 DevTools 自己删。Claude 桌面版的内置浏览器已经给出了一套成熟做法:「保留 Cookie」三档(退出即清 / 共享 / 按会话)、按档位清除浏览数据、删除会话时清掉它的 cookie、分叉会话继承 cookie。ACECode 要按同一套语义复刻,让用户既能长期保持登录(不用每次重输用户名密码),又能在需要时隔离或清空登录态。

## What Changes

- Agent Browser 增加「保留 Cookie」设置,三档:
  - **退出即清**(对应 Claude 的 Until quit):cookie 只保留到 Desktop 退出。
  - **共享**(对应 Shared,**默认**):所有会话共用一份长期保存的 cookie。沿用现有 Profile,升级后已有登录不丢失。
  - **按会话**(对应 Per session):每个会话(子代理随其根会话)各存一份,互相看不到对方的登录。
- 浏览器工具栏新增「更多」菜单,含「保留 Cookie」子菜单和「清除浏览数据」;「设置 > Agent 浏览器」里也能改档位。清除的范围随档位变化,执行前二次确认。
- 切换档位时,已打开的页面按原 URL 在新档位的 cookie 存储里重建,页面 id 不变。
- 切到「退出即清」时,立即删除所有已长期保存的 cookie(共享的和各会话的);以「退出即清」启动时也会补清一次,覆盖上次异常退出遗留的数据。
- 「按会话」档下:永久删除会话时删除它那份 cookie(归档不删);分叉出的会话第一次打开浏览器时复制父会话的 cookie。
- 会话的删除与分叉发生在 daemon,cookie 存储在 Desktop。daemon 把「待删除」「待复制」记录写进共享状态文件,Desktop 启动时和收到提醒时处理,所以 Desktop 没开时删除的会话也会在下次启动时清掉。提醒走现有 Agent Browser 代理的一个新操作,失败不影响任何功能,协议版本不变,新旧版本混用不会断连。
- 不做:从 Chrome / Edge / Firefox 导入 cookie;把没有过期时间的会话 cookie 改成长期保存。Claude 也不保存会话 cookie,Electron 的落盘会话写死了 `persist_session_cookies = false`。

## Capabilities

### New Capabilities

- `agent-browser-cookie-storage`:Agent Browser 的 cookie / 网站数据存储策略。覆盖三档存储模式及默认值、档位持久化、切换档位时的页面重建与清理、按档位清除浏览数据,以及会话删除与分叉对 cookie 存储的影响。

### Modified Capabilities

(无。仓库主 spec 中没有 Agent Browser 的 capability;页面归属规则在 `bind-agent-browser-pages-to-sessions` 变更的 `agent-browser-page-ownership` 中,本变更直接复用其中的 `session_id` / `root_session_id`,不改动它的要求。)

## Impact

- **Windows 原生宿主**(`src/apps/desktop/agent_browser_host.cpp`):页面创建改用 `CreateCoreWebView2ControllerWithOptions` 并按档位设置 `ProfileName` / InPrivate;用 Profile 接口清除数据、删除 Profile、复制 cookie;新增一个代理操作。
- **macOS 原生宿主**(`src/apps/desktop/agent_browser_host_mac.mm`):按档位选择 `nonPersistentDataStore`、现有固定 UUID 的 store、或按会话派生 UUID 的 store;对应的清除、删除与 cookie 复制。
- **新纯逻辑模块**(`src/apps/desktop/agent_browser_cookie_jars.{hpp,cpp}`):档位解析、存储键与 Profile 名派生、切档 / 清除 / 删除的判定,两端宿主共用,可单测。
- **共享状态模块**(`src/base/ipc/agent_browser_store_state.{hpp,cpp}`):`state.json` 里 `agent_browser` 段的读写,Desktop 与 daemon 共用。
- **Desktop bridge**(`src/apps/desktop/main.cpp`):读写档位、清除浏览数据。
- **daemon / TUI**:会话层新增「分叉成功」通知(与现有的永久删除通知并列),新服务监听这两个通知并写入待处理记录(`src/domain/session/`、`src/host/session_host/`)。
- **前端**:`web/src/components/AgentBrowserPanel.jsx` 工具栏菜单、设置页 Agent 浏览器分节、新纯逻辑 `web/src/lib/agentBrowserCookieMode.js`、`agentBrowser.js` bridge 封装、i18n 目录。
- **状态文件**:`state.json` 新增 `agent_browser` 段(档位、按会话存储清单、待删记录)。
- **文档**:`docs/agent-browser.md`、`docs/help/agent-browser.html`、`CLAUDE.md` 相应段落。
