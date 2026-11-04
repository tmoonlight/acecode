# 对话内可视化与画图说明

模型在回复正文里写 ```` ```visual ```` 代码块（一段 SVG / HTML 片段），Web / Desktop 把它画在对话中的隔离框里：边写边长出来，代码块闭合后才执行其中的脚本。系统提示里常驻一段「画图说明」，告诉模型界面能画什么、什么时候画、什么时候不画；mermaid 长期不触发也是同一个原因（模型不知道界面能渲染它），一起由这段说明解决。设计取舍见 `openspec/changes/add-inline-visuals/design.md`。

## 各处怎么显示

| 位置 | 顶层 visual 代码块 | 嵌套在列表 / 引用里的 visual |
|---|---|---|
| Web / Desktop 主对话 | 隔离框（[InlineVisualBlock.jsx](../web/src/components/InlineVisualBlock.jsx)） | 普通代码块 |
| 文件预览、侧边对话等其它 Markdown | 普通代码块（`renderMarkdown` 不识别） | 普通代码块 |
| TUI | 一行占位提示（[markdown_formatter.cpp](../src/apps/tui/markdown/markdown_formatter.cpp)） | 普通文本 |
| IM 消息通道 | 换成占位提示后发出（`classify_session_event`） | 原样 |

「顶层」的判定 Web 用 markdown-it 的 token 层级（[markdown.js](../web/src/lib/markdown.js) 的 `renderMarkdownBlocks`），C++ 用 [inline_visual_blocks.cpp](../src/domain/llm/inline_visual_blocks.cpp)：行首最多 3 个空格、不在引用块与列表项里，信息串第一个词是 `visual`（不区分大小写）。其它语言代码块里的 ```` ```visual ```` 字样不算。

## 画图说明（系统提示）

[diagram_guidance.cpp](../src/engine/prompt/diagram_guidance.cpp) 生成固定文本，`ApiRequestBuilder::static_system_prompt` 追加在静态系统提示末尾（loop / 群聊代答段之前）。

| 会话 | mermaid 部分 | visual 部分 |
|---|---|---|
| Web / Desktop（daemon worker 设 `DiagramSurface::Web`） | 有 | 「可视化图表」开关打开时有 |
| TUI（`TuiApp::prompt_config_snapshot` 设 `Terminal`） | 有（写明终端里是文字图） | 无 |
| headless `-p`（不设，`None`） | 无 | 无 |
| spawn_subagent 子会话、网状子 agent（有父会话 id） | 无 | 无 |
| 群聊代答会话 | 无 | 无 |

- 显示面经 `SessionPromptConfig` 进入请求；子会话与群聊代答由 `RequestContextFactory::options` 降为 `None`。
- **字节稳定**：文本只由显示面、开关、画图规范技能是否可用、`skill_view` 的模型侧名字决定，不含时间、路径或会话信息。只有切换开关、技能被停用 / 恢复、「工具重写」改了 `skill_view` 的名字时才变化。`RequestContextSource` 每回合捕获一次，所以同一回合里的多次请求一定相同。
- **mermaid 部分**只列 Web（`mermaidRenderer.js` 的 `inspectMermaidSource`）与 TUI（`mermaid_renderer.cpp`）都能渲染的五种类型，并写明会让图退回源码的写法。改这两个渲染器的白名单或拒绝规则时同步改这段文字。
- **visual 部分**写核心约束（顶层、片段、主题变量、脚本在闭合后执行、四个 CDN、`ace.send`）。画图规范技能可用（本会话技能快照里有 `inline-visuals`，且工具策略允许 `skill_view`）时，要求第一次画之前用 `model_tool_name_for_native("skill_view")` 的名字加载它；不可用时只给核心约束。
- 测试：[diagram_guidance_test.cpp](../tests/prompt/diagram_guidance_test.cpp)（文本取舍）、[diagram_guidance_request_test.cpp](../tests/agent/request/diagram_guidance_request_test.cpp)（请求组装、子会话降级、技能索引隐藏）。

## 画图规范技能 `inline-visuals`

内置在 [SKILL.md](../assets/seed/skills/acecode/inline-visuals/SKILL.md)，随 `assets/seed` 版本号由默认技能播种器补齐。完整规范有数千 token，放系统提示会让每次请求都多带一大段，所以做成技能按需加载。

- visual 部分不生效的会话（TUI、headless、开关关闭、子会话）把它并进技能索引的隐藏集合（`ApiRequestBuilder::hidden_index_skills`，与休眠技能同一个入口），技能目录身份按同一规则计算。
- 生效时即使它因长期未用进入休眠、不在索引里，`skill_view` 仍能按名字加载。
- 技能里写给模型的变量名、类名、标记 id、`ace` 接口与 CDN 清单必须与隔离框外壳一致，`inlineVisualArchitecture.test.js` 逐项核对。

## 「可视化图表」开关

设置 > 常规，daemon 配置 `inline_visuals.enabled`（默认开启，写配置时省略默认值），接口 `GET / PUT /api/config/inline-visuals`（见 [daemon-api.md](daemon-api.md#inline-visuals)）。关闭只影响模型是否被告知可以画 visual；历史里已有的 visual 照常显示，mermaid 说明不受影响。Desktop 其它工作区的 daemon 在下一次发消息前经 `refresh_model_config_if_changed` 从 config.json 跟上。

## 隔离框

### 安全边界

- iframe 只有 `sandbox="allow-scripts"`：不透明源，读不到 ACECode 页面、存储与 Cookie；不能弹窗、提交表单、导航外层页面或弹模态框。
- srcdoc 的 `<head>` 第一个元素是内容安全策略：`default-src 'none'`；脚本 / 样式 / 字体 / 网络只放行 cdnjs.cloudflare.com、cdn.jsdelivr.net、unpkg.com、esm.sh；图片与媒体只能 `data:` / `blob:`；`form-action 'none'`、`base-uri 'none'`。本机请求免 token，所以必须由它挡住对 daemon 的 fetch / XHR / WebSocket / 图片请求。daemon 另外拒绝 `Origin: null` 的跨源请求。
- srcdoc 会继承父页面的内容安全策略。ACECode 页面以后若加 CSP，必须允许 srcdoc 里的内联脚本，否则隔离框失效。
- 框内运行时丢掉 `meta`、`base`、`iframe`、`object`、`embed` 与非样式表的 `link`；拦截所有链接点击与 `window.open`。srcdoc 从不改变，再次 load 后 2 秒内运行时没有重新报到，就当作被导航走了，改为显示源码。
- 外层只处理 `event.source === iframe.contentWindow` 的消息。追问与链接还要求本页面处于用户激活状态（框内的点击会沿父级传递激活，框内脚本伪造不了；没有 `navigator.userActivation` 的旧 WebKit 退而要求焦点在这个 iframe 上）。追问每框 1 秒一次、最长 4000 字符；链接只放行 http / https / mailto，交给 `openExternalUrl`（Desktop 用系统浏览器）。

### 消息协议

所有消息都带 `ace_visual: 1`。框内运行时是 [inlineVisualRuntime.js](../web/src/lib/inlineVisualRuntime.js) 里一个不引用外部变量的函数，由 [inlineVisualFrame.js](../web/src/lib/inlineVisualFrame.js) 用 `Function.prototype.toString` 拼进 srcdoc；函数体里不能写中文字符串字面量（构建期文案本地化插件会把它们换成框里不存在的 `__acecodeT` 调用）。

| 方向 | 类型 | 内容 |
|---|---|---|
| 外层 → 框内 | `render` | `code`、`final`。流式时剥掉全部 `<script>` 与 `on*` 属性，按位置差量对齐到 `#ace-root`；`final` 时最后对齐一次，按文档顺序执行脚本（外链脚本等加载完或 15 秒超时再执行下一个，失败显示一行提示），再补发 `DOMContentLoaded` 与 `load`。定稿后的 `render` 一律忽略 |
| 外层 → 框内 | `theme` | `mode`、`vars`（框内短变量）、`palette`。只换变量与 `data-mode`，不重建内容、不重跑脚本，回调 `ace.onThemeChange` |
| 框内 → 外层 | `ready` | 运行时启动完成。外层此时补发当前主题与最新一次 `render`（也是重载后的恢复路径） |
| 框内 → 外层 | `size` | `height`。外层夹到 40–4000px，并按代码内容记住高度，重新挂载时先用 |
| 框内 → 外层 | `send` | `text`，见上面的追问规则。会话空闲时与「下一步」框按钮同一路径发出，忙时追加进输入框 |
| 框内 → 外层 | `link` | `url` |
| 框内 → 外层 | `error` | `message`，只写到控制台 |

`ready` 之前外层不发消息，只记住最新一次 `render`；消息监听在提交阶段（`useLayoutEffect`）挂上，首次 load 时若还没收到 `ready` 也补一次握手。定稿后内容若又变化（重新生成等），组件换一个新 iframe 按新内容重画。

隔离框能从流式用到定稿，前提是它所在的对话行不被卸载：`Message.jsx` 按块序号做 key，对话行按条目 id 做 key。回合结束后的对话记录自愈（`transcriptSelfHeal.js`）替换最新一轮时，按位置对得上的行沿用原 id（`withStableIds`）；如果整轮换新 id，每个隔离框都会重新加载、脚本重跑。

### 主题与辅助类

外层从 `<html>` 的计算样式读 `--ace-*`，映射成 `--fg` `--fg-2` `--fg-3` `--surface` `--surface-2` `--border` `--border-strong` `--accent` `--accent-bg` `--ok` `--ok-bg` `--warn` `--warn-bg` `--danger` `--danger-bg` `--font` `--mono`（`--fg-3` / `--border-strong` 由 `--fg` 推出），`data-mode` 跟随 `data-theme`；MutationObserver 盯 `<html>` 的属性变化后推送。色调类 `tone-*` 只设置 `--tone-bg` / `--tone-line` / `--tone-ink`，`node` `edge` `t` `card` `badge` 等规则读这些变量。预置 `#arrow` / `#dot` 标记（颜色先写固定值，再用 `context-stroke` 跟随连线）。iframe 元素的 `color-scheme` 必须与框内 `data-mode` 一致，否则浏览器会给透明的框垫一层不透明底色。

## 测试

- C++：`config_inline_visuals_test.cpp`、`default_session_preferences_test.cpp`、`inline_visuals_handler_test.cpp`、[inline_visual_blocks_test.cpp](../tests/llm/inline_visual_blocks_test.cpp)、`session_channel_binder_test.cpp`、`markdown_formatter_test.cpp`、上面两个画图说明测试、`DefaultSkillSeedRegistryTest.PackagedManifestVersionAndHashesAgree`。
- 前端：`inlineVisualFrame.test.js`（CSP、外壳、主题、消息校验）、`inlineVisualRuntime.test.js`（xmldom 假 DOM 上的对齐、剥离、追问门槛，以及运行时在空上下文里独立求值）、`inlineVisualArchitecture.test.js`（sandbox、来源校验、与技能 / 系统提示的一致性）、`markdownBlocks.test.js`、`composerContent.test.js`、`inlineVisuals.test.js`。
