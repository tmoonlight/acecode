# Design: add-inline-visuals

## Context

动机见 proposal.md。与实现方式相关的现状:

- **同类产品的做法**:Claude app 走 MCP Apps(工具调用 + `ui://` HTML 资源),宿主把模型还没写完的工具参数推给隔离框增量渲染。ACECode 只要效果,不需要 MCP 那层通道。
- **ACECode 现状**:`ToolCallDelta` 事件故意不携带写了一半的参数(`llm_provider.hpp`),所以工具方案要想边写边画,得改 4 个 provider。正文 token 本来就逐字推到前端;流式时 `renderMarkdownBlocks(..., {streaming:true})` 已经把最后一个未闭合的代码块单独交给 `Message.jsx` 渲染(`openFence`)。`useStreamingRenderThrottle` 把重渲染限到 100ms 一次。
- **系统提示**:静态部分由 `ApiRequestBuilder::static_system_prompt` 拼出,`<loop-execution>` / `<group-delegate>` 已经按 `RequestContextOptions` 追加在末尾。会话级配置经 `SessionPromptConfig` 快照进入(daemon 由 `SessionRegistry::prompt_config_snapshot` 在共享配置锁下读 `AppConfig`,TUI 由 `TuiApp::prompt_config_snapshot` 提供,headless 也走 SessionRegistry)。
- **mermaid**:Web(`mermaidRenderer.js` 的 `inspectMermaidSource`)和 TUI(`mermaid_renderer.cpp`)都只渲染 flowchart/graph、sequence、state、class、er 五种;Web 还会拒绝 `%%{`、front matter、`<br>` 以外的 HTML、链接 / URL、`click`、flowchart 不写方向等写法。
- **daemon 没有设置 CSP**,`index.html` 也没有;`PresentationPreview.jsx` 已经在用 `sandbox="allow-scripts"` + `srcDoc`。
- **本机请求免 token**:页面里任何脚本都能直接请求本机 daemon,隔离必须挡住这一点。

## Goals / Non-Goals

**Goals:**

- 模型输出期间,图在原位逐步出现,同一个隔离框从流式一直用到定稿。
- 隔离框里的代码碰不到 ACECode 页面、daemon 与本机网络。
- 画图说明对 prompt 缓存零额外成本(只在开关和运行面变化时改变)。
- C++ 侧不改 provider,不改工具协议。

**Non-Goals:**

- 嵌套在 Markdown 容器里的 visual、非对话场景(文件预览、侧边对话)的渲染。
- 离线图表库、导出图片、全屏。

## Decisions

### D1 用正文代码块,不用工具调用

模型在回复正文里写 ```visual 代码块,前端识别后渲染。

- **为什么**:正文本来就逐字推到前端,边写边画不用改 provider;历史回放直接用落盘正文;整段 HTML 不用转义进 JSON 字符串,省 token,也避开部分网关把超长工具参数写坏的问题;任何不认识 visual 的地方都能自然退化成代码。
- **放弃的方案**:内置 `show_widget` 工具 + 转发写了一半的参数,要改 4 个 provider 和事件协议;MCP Apps 宿主,ACECode 没有第三方 UI 的需求,多一层握手、resources/read 与 MCP 管理器改动。
- **代价**:没有工具结果那一步来告诉模型「已经画好,别再用文字重复」,改由画图说明写明;复制整条回复会带上源码,可以接受。

### D2 只渲染顶层代码块,并且用同一个 React 组件贯穿流式与定稿

`renderMarkdownBlocks` 对顶层 visual 围栏(无论是否闭合)返回 `{ key, visual: { text, closed } }`;`Message.jsx` 对这类块渲染 `InlineVisualBlock`,`key` 用块序号。块在流式与定稿期间序号不变,React 复用同一个组件实例,iframe 也就不会重建。嵌套的 visual 围栏走 markdown-it 的 fence 规则,按普通代码块输出。

- **放弃的方案**:像 mermaid 那样输出 HTML 占位、再由全局扫描器挂载。流式到定稿要从 React 文本节点切到 HTML 字符串,iframe 必然重建,图会闪一下,脚本状态也会丢。
- **实现中发现**:回合结束后的对话记录自愈(`transcriptSelfHeal.js`)只要权威历史与实时记录有细微差异就替换最新一轮,原先整轮换新 id,对话行按 key 卸载重建,隔离框因此重新加载、脚本重跑。改为按位置对得上的行(种类、角色一致,工具还要同一个调用 id)沿用原 id,内容照样换成权威版本。

### D3 隔离用无同源权限的 srcdoc iframe 加内容安全策略

- iframe 只加 `sandbox="allow-scripts"`,不允许同源、弹窗、表单、顶层导航和模态框,文档是不透明源,读不到外层页面、存储和 Cookie。
- srcdoc 的 `<head>` 第一行放 CSP:`default-src 'none'`;script / style / font / connect 只放行四个 CDN(script 另加 `'unsafe-inline' 'unsafe-eval'`,style 另加 `'unsafe-inline'`);img / media 只放行 `data:` `blob:`;`form-action 'none'`;`base-uri 'none'`。不放行本机,于是 fetch / XHR / WebSocket / 图片都打不到 daemon。
- 外层只处理 `event.source === iframe.contentWindow` 的消息,其余一律忽略。
- **放弃的方案**:Claude 那样用独立的沙箱域名。ACECode 是本机 daemon 单一来源,Desktop 每个工作区还各有一个端口,再开一个来源的代价远大于收益;不透明源加 CSP 已经能把本机挡住。
- **约束**:srcdoc 文档会继承父页面的 CSP。以后 ACECode 页面若加 CSP,必须允许 srcdoc 里的内联脚本,否则隔离框失效。架构测试守住「sandbox 恰好是 allow-scripts、srcdoc 以 CSP 开头」。

### D4 外壳只生成一次,之后靠消息更新

- 外壳 HTML 包括 CSP、基础样式、主题变量、预置箭头 `<marker id="arrow">`、框内运行时。每个隔离框只生成一次,初始主题直接写进去;之后主题变化、内容更新都用 postMessage 发进去,所以 srcdoc 永远不变,iframe 不会重新加载。
- 框内运行时写成一个不引用外部变量的函数,用 `Function.prototype.toString` 拼进 srcdoc。这样 Node 测试可以直接导入,也不需要 Vite 的 `?raw`。
- 消息协议(都带 `ace_visual: 1` 标记):外层 → 框内是 `render {code, final}` 与 `theme {mode, vars, palette}`;框内 → 外层是 `ready`、`size {height}`、`send {text}`、`link {url}`、`error {message}`。iframe `load` 之前发出的消息先排队。

### D5 流式差量更新用自写的小型 DOM 对齐

- 流式内容几乎只在末尾追加,按位置对齐子节点就够用:同类型节点更新属性和文本并递归,不同就替换,多出的删掉。没有为此引入 morphdom 依赖。
- 每次更新先把 `<script>` 整段剥掉(含末尾写了一半的),再用 `<template>` 解析。
- 定稿时先做最后一次对齐,再把每个 script 节点换成新建的 script 让它执行:外链脚本串行等加载完,失败就跳过并报 error。全部执行完补发一次 `DOMContentLoaded` 与 `load`。LLM 写的页面常把初始化挂在这两个事件上,外壳加载时它们已经触发过了。
- 框内用内存实现顶替 `localStorage` / `sessionStorage`(不透明源访问它们会抛错),顶替失败就算了。

### D6 主题变量与 SVG 辅助类

- 外层从 `document.documentElement` 的计算样式读 `--ace-*`,映射成框内的短变量:`--fg` `--fg-2` `--fg-3` `--surface` `--surface-2` `--border` `--border-strong` `--accent` `--accent-bg` `--ok` `--ok-bg` `--warn` `--warn-bg` `--danger` `--danger-bg` `--font` `--mono`,外加 `data-mode="light|dark"`。用 MutationObserver 盯 `<html>` 的 `data-theme` / `style` / `class`,有变化就重新取值推进去。
- 色调类用自定义属性实现:`.tone-blue` 等只设置 `--tone-bg/--tone-line/--tone-ink`,形状与文字的规则读这些变量,所以离得最近的那层色调生效,嵌套不会出错。浅色 / 深色两套取值各一份,数值取自 Tailwind 调色板(MIT)。
- 另给 canvas 图表一个 `ace.palette`(8 个解析好的十六进制色),Chart.js 读不了 CSS 变量。
- 箭头标记颜色先写一个固定值,再写 `context-stroke` 作后备;不支持 `context-stroke` 的 WebKit 就沿用固定色。

### D7 追问桥与链接

- 框内 `ace.send` 只在用户刚操作过时生效:优先看 `navigator.userActivation.isActive`;没有这个接口时,看 1 秒内是否有 `isTrusted` 的点击 / 按键 / 输入事件。
- 外层再把一道关:每个框 1 秒最多一次,文本限 4000 字符。会话空闲时走与「下一步」框按钮相同的发送路径;忙时只填进输入框,输入框已有内容就换行追加。
- 链接一律走现有的 `openExternalUrl`(Desktop 用系统浏览器),只放行 http / https / mailto。

### D8 画图说明放在静态系统提示末尾

- 新增 `engine/prompt/diagram_guidance`,输入是运行面(none / terminal / web)、visual 开关、画图规范技能是否可用、skill_view 的模型侧名字,输出固定文本。`ApiRequestBuilder::static_system_prompt` 在 `build_system_prompt` 结果后面追加(与 loop / delegate 同一个位置)。这样不用改 `build_system_prompt` 的签名,也不动它的几十处调用和逐字节测试。
- 运行面由宿主写进 `SessionPromptConfig`:worker 设为 web,TUI 设为 terminal,headless 不设(none)。visual 开关从同一份 `AppConfig` 快照读。
- `RequestContextFactory` 发现会话有父会话(子代理 / 网状子 agent)或是群聊代答时,把运行面降为 none。
- 说明是英文(系统提示其余部分都是英文),只列两端 mermaid 渲染器都支持的写法,并与 `inspectMermaidSource` 的拒绝规则对齐。

### D9 画图规范是内置技能,不进系统提示

- 完整规范有数千 token,放进系统提示会让每次请求都多带一大段,上下文窗口小的模型更吃亏。做成内置技能 `inline-visuals`,第一次画之前按名字加载一次。
- 技能可用 = 本会话的技能快照里有它(没被停用),并且工具策略允许 skill_view。
- visual 部分不生效时,把它并进技能索引的隐藏集合(与休眠技能同一个入口),免得 TUI 里的模型看到它就去画 visual。skill_view 按名字查找,不看休眠状态,所以休眠后仍能加载。

### D10 开关与跨 daemon 同步

- `AppConfig::inline_visuals { bool enabled = true; }`,写配置时省略默认值。
- `GET / PUT /api/config/inline-visuals` 仿照 `/api/config/image-compression`:纯函数 handler 负责快照与 patch 解析,路由在写锁里 `mutate_config` 落盘。daemon 的提示配置快照每次请求都读 `AppConfig`,所以本进程立即生效。
- 同机其它 daemon 由 `refresh_model_config_if_changed`(下一次发消息前、`GET /api/models` 时都会调)从磁盘读回 `inline_visuals.enabled`,与「压缩图片」同一处。

### D11 降级显示

- `domain/llm/inline_visual_blocks` 提供纯函数:识别顶层 visual 围栏,把它替换成给定的占位文字。IM 在 `classify_session_event` 的助手文本分支里调用,新版消息通道和旧网关都会经过这里。
- TUI 的 Markdown 代码块分支遇到 `visual` 时,输出一行灰色占位提示。

## Risks / Trade-offs

- [不同模型画出来的质量差别大] → 结构类图优先 mermaid;规范技能里写清布局算法和反例;说明里写明什么时候不画。
- [一张图上万 token] → 说明要求只在明显比文字更清楚时才画;边写边画让等待过程可见。
- [离线或受限网络连不上 CDN] → 规范要求先用纯 SVG / HTML,图表库只是可选项;外链脚本加载失败时框内显示一行加载失败提示,不影响其余内容。
- [模型写 `position:fixed` 或超高内容] → 外层把高度限制在 40–4000px 之间;规范里禁止 fixed。
- [长会话里 iframe 很多] → 对话尾部窗口本来就限制了 DOM 数量;iframe 用 `loading="lazy"`;记住高度,滚回时不跳。
- [被注入的内容诱导模型画出恶意 visual] → 隔离框没有网络和数据可偷;追问只在用户亲手点击时发出,文本对用户可见。
- [升级后 prompt 缓存全量计算一次] → 一次性成本,可以接受。
- [macOS Desktop 的 WebKit 缺少某些接口] → `userActivation` 与 `context-stroke` 都有后备;隔离框依赖的 sandbox / srcdoc / CSP WebKit 都支持。

## Migration Plan

- 默认开启,无数据迁移。旧客户端没有设置项也不影响(配置缺省即开启)。
- 回退:关闭「可视化图表」即可停止让模型画 visual;mermaid 说明不受开关控制,代码回退会一起去掉。
- 内置技能随 `assets/seed` 版本号升级,由默认技能播种器在启动时补齐。
