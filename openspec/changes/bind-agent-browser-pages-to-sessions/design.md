<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

Desktop 只有一个 `AgentBrowserHost`，服务所有工作区的 daemon；每个页面在 host 里是 `Page`，状态经 `acecode:agent-browser-state` 推给主 WebView。Web UI 的浏览器页签存在 ChatView 的内存 state（`browserTabsBySession`），刷新与切工作区（整页导航）后清零。引入本变更前，页面 ↔ 会话的绑定只存在于 ChatView 的一次瞬时推断里，四种情况下断裂：工具执行时没在看该会话、工具结束后再回来、刷新 / 切工作区、子代理与后台会话。

## Goals / Non-Goals

**Goals:**

- 页面归属成为 host 里的一等数据，daemon、Desktop、Web UI 三方以 `session_id` 对账。
- 后台会话的 `browser_open` 不改变用户正在看的页面；切回该会话时页签存在并被激活。
- 省略 `page_id` 的工具只落到本会话的页面；显式 `page_id` 仍可跨会话操作共享页。
- 刷新 / 切工作区后能从 native 页面池找回页签。
- 旧 daemon / 旧 Desktop 组合下行为退回引入归属前，不新增崩溃面。

**Non-Goals:**

- 不改 WebView2 / WKWebView 的后台渲染策略；非显示页面是否总能出帧仍按站点实测。
- 不在 Desktop 重启后复活页面（页面随 Desktop 进程消亡）。
- 不做「把页签移动到另一个会话」的 UI。

## Decisions

### 1. 归属簿记是两端 host 共用的纯逻辑

新增 `AgentBrowserPageDirectory`（`src/desktop/agent_browser_page_directory.{hpp,cpp}`）持有页序、owner、显示页与每会话 Agent 目标页；Windows 与 macOS host 用它替换各自的 `page_order` + `active_page`。它不依赖平台 API，可在跨平台单测里覆盖解析规则与关闭回退。备选是在两个 host 里各写一套映射，重复且 macOS 无法在本机验证，排除。

### 2. 显示页与 Agent 目标页分离；带 owner 的建页不抢显示

旧模型的单一「活动页」同时承担「UI 正在显示哪一页」与「工具默认操作哪一页」，任一方改动都会波及另一方。现在显示页只由 UI 的 select 或关闭回退设置；`create_page_on_ui(shared, owner, agent_created)` 只在 `!agent_created || owner.empty()` 时 select——旧协议与 UI 自建页保持「新页即显示」。UI 侧当前显示的会话有新页面出现或 Agent 目标页变化时，由 ChatView 主动激活对应页签并 select，所以体验上「Agent 自动激活」不变，只是决策权从 host 移到了知道当前会话是谁的 UI。

### 3. 联结键只有 session_id

`workspace_hash` 在 junction 与 no-workspace 会话下两侧形态未必一致（daemon 由 `projects/<hash>` 目录名取，前端 no-workspace 会话的 workspaceHash 为空），因此只作附带信息；`root_session_id` 让子代理页面在父会话页签可见，权限与默认目标仍按子会话自身。

### 4. 省略 page_id 的解析顺序

有 owner：本会话目标页 > 当前显示页（仅当它也属于本会话）> 新建。第二级保住「用户手工开页并共享后让 AI 读这页」的直觉用法。无 owner（旧协议）：显示页 > 新建。隐式 claim 与显式 select 成功后把本会话目标钉到那一页，保持旧的粘性锁定语义。

### 5. Web UI 页签只有一个来源：App 级登记表

`lib/agentBrowserPages.js` 用 `createSingleWriterStore` 镜像 native 状态事件，`App.jsx` 挂载时安装监听并全量对账（`aceDesktop_agentBrowserListPages`），ChatView 切会话再按会话对账；页签由 `syncBrowserTabsForSession` 派生。ChatView 按会话记录已揭示过的页面，首次见到的页面自动打开并激活，再次切回不重复抢焦点。活动推断（`agentBrowserActivityFromItems`）只决定彩虹边框与「Agent 切目标页时前置该页签」。

### 6. 协议版本升级而不是字段嗅探

`kAgentBrowserRuntimeProtocolVersion` 4→5，manifest 校验拒绝版本不一致的组合；同一版本内 `owner` 仍是可选字段，缺省即旧行为。旧 Desktop 事件不带 owner 时前端本地认领（`claimUnownedAgentBrowserPage`），且不会覆盖 native 已归属的页面。

## Risks / Trade-offs

- [非显示页面被 Chromium 节流] → 显示与否仍由 UI 决定；后台会话截图是否总能出帧需实测，失败时工具返回明确错误而不是静默空图。
- [显式 page_id 跨会话操作会把本会话目标钉到别的会话的页] → 这是旧的粘性锁定语义的延续，结果里回报 `page_id`，UI 的页签仍按 native owner 展示。
- [macOS 端只能编译期审查] → 变更与 Windows 端逐段对称，mac smoke 保留原用例；架构测试对两端 host 同时断言关键行。
- [同一 ChatView 实例首次见到旧页面会激活它] → 这是用户期望（切回去页签就在），仅在本视图首次见到时触发一次。

## Migration Plan

1. 先落纯逻辑簿记与单测，再替换两端 host 的簿记。
2. 接协议（owner 字段 + 版本 5）、daemon ToolContext 与 cdp_client。
3. 换前端登记表与页签派生，更新架构测试。
4. 构建 Desktop、跑单测与 Windows smoke（`SMOKE_OWNERSHIP_OK`），前端 test/build。

回滚整体回退本变更，没有持久化数据迁移。
