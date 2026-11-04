# Proposal: add-inline-visuals

## Why

用户希望 ACECode 也能像 Claude app 的「make a visual」那样,在对话里直接画出可交互的示意图、图表和对比卡片,而且图要随着模型输出一点点长出来。另外 mermaid 图长期「不触发」:系统提示和内置技能里从没说过界面能渲染 mermaid。本机全部历史只出现过 30 个 mermaid 代码块,最近 30 天的 2344 个会话里一个都没有;用户明确要图的 10 次里有 4 次没给。两件事是同一类问题:模型不知道自己能画,所以要一起解决。

## What Changes

- **可视化代码块**:模型在普通回复里写语言为 `visual` 的代码块,内容是一段 SVG 或 HTML 片段。Web / Desktop 把顶层的这种代码块画在对话里的隔离框中,模型边写边更新,代码块闭合后才执行其中的脚本。不走工具调用,因此不用改 provider 的流式协议,历史回放直接从落盘正文重画。
- **隔离与安全**:隔离框是只允许脚本、没有同源权限的 iframe,内容里固定加一条内容安全策略,只放行几个公共 CDN 的脚本与样式,禁止访问本机 daemon 和任意外部地址。框内点击链接交给系统浏览器打开;框内的 `ace.send(text)`(兼容 `sendPrompt`)只在用户真实点击时以用户身份发出追问,会话忙时改为填进输入框。
- **主题与尺寸**:框内提供跟随 ACECode 主题的颜色变量、SVG 辅助类和预置箭头,浅色 / 深色切换不重建内容;高度随内容自动撑开,不出现内层滚动条。框右上角有复制源码按钮。
- **画图说明常驻系统提示**:新增一段固定的「画图说明」,告诉模型界面能画什么、什么时候画、什么时候不画。mermaid 部分在 TUI 和 Web 都生效,只列两端都能渲染的五种类型和前端安全检查会拒绝的写法;visual 部分只在 daemon(Web / Desktop)且开关打开时出现。子代理、群聊代答和 headless 会话不加这段。说明只随开关和运行面变化,不打穿 prompt 缓存前缀。
- **详细规范放内置技能**:新增内置技能 `inline-visuals`,写清楚隔离框提供的变量与类名、SVG 布局与配色规则、交互与脚本规则、图表写法和示例。系统提示要求第一次画 visual 前先加载它;visual 部分不生效时,它也不出现在技能索引里。
- **开关**:设置 > 常规新增「可视化图表」(daemon 配置 `inline_visuals.enabled`,默认开启),对应 `GET/PUT /api/config/inline-visuals`。关闭后模型不再被告知可以画 visual;已有的 visual 仍然正常显示。Desktop 的其它工作区 daemon 在下一次发消息前从 config.json 跟上。
- **看不到图的地方用占位文字**:TUI 把 visual 代码块显示成一行提示;IM 消息通道发出前把它换成一句「请在桌面版或网页中查看」的提示;嵌套在列表 / 引用里的 visual 代码块,以及文件预览等其它 Markdown 场景,按普通代码块显示。

## Capabilities

### New Capabilities

- `inline-visuals`:visual 代码块的识别、隔离框渲染、流式更新、主题与尺寸、框内交互桥、开关,以及 TUI / IM / 嵌套场景的降级显示。
- `diagram-guidance`:系统提示里的画图说明(mermaid 与 visual 两部分、按运行面与开关取舍、字节稳定)以及画图规范技能的加载与隐藏规则。

### Modified Capabilities

(无。现有 `webui-rich-rendering` 的「Markdown 里的原始 HTML 一律转义」不变:visual 的内容不在对话文档里解析,只在隔离框内渲染。)

## Non-goals

- 不实现 MCP Apps 协议,也不支持第三方 MCP 服务器提供界面。
- 不在本地打包 Chart.js / d3 等图表库;离线或受限网络连不上 CDN 时,规范引导模型用纯 SVG / HTML 画。
- 不做全屏、导出 PNG、分享,也不在侧边对话窗口里渲染 visual。
- TUI 不渲染 visual;不扩大 mermaid 前端安全检查的类型白名单。

## Impact

- **C++**:`config`(`inline_visuals` 段的加载 / 保存 / 跨 daemon 刷新)、`SessionPromptConfig` 与 daemon / TUI / headless 的提供方、请求上下文与静态系统提示的拼接、`engine/prompt` 新增画图说明生成、技能索引隐藏规则、`/api/config/inline-visuals` 路由与纯函数 handler、IM 出站文本处理、TUI Markdown 代码块分支、内置技能清单与 `assets/seed`。
- **Web**:新增隔离框外壳(srcdoc 构建、框内运行时、消息协议、主题变量)与 `InlineVisualBlock` 组件;`markdown.js` 分块、`Message.jsx`、`ChatView.jsx`(发送桥)、设置页开关、设置搜索索引、`api.js`、i18n 目录。
- **Prompt 缓存**:升级后第一次请求、以及切换开关后的第一次请求会全量计算一次,之后前缀稳定。
- **文档**:`docs/daemon-api.md` 新路由,`docs/inline-visuals.md` 主题文档,`CLAUDE.md` 加一行索引。
