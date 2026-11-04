# inline-visuals Spec (delta)

## Purpose

让模型在 Web / Desktop 对话里用 `visual` 代码块画出可交互的示意图、图表与卡片:在隔离框里边写边画,跟随主题,并在 TUI、IM 等画不了图的地方给出占位提示。

## ADDED Requirements

### Requirement: 识别 visual 代码块
Web / Desktop 对话中,助手消息里位于顶层、信息串第一个词为 `visual`(不区分大小写)的围栏代码块 SHALL 渲染为可视化隔离框,而不是代码块。嵌套在列表、引用等容器里的 visual 代码块,以及文件预览、侧边对话等其它 Markdown 场景,SHALL 按普通代码块显示。其它语言(含 `html`、`svg`)的代码块 MUST 保持原样。

#### Scenario: 顶层 visual 代码块
- **WHEN** 助手回复正文里有一段顶层的 ```visual 代码块,内容是 `<svg>…</svg>`
- **THEN** 对话里在该位置显示一个隔离框并画出这张 SVG,不显示源码

#### Scenario: html 代码块不受影响
- **WHEN** 用户让模型写网页代码,回复里是 ```html 代码块
- **THEN** 仍按高亮代码块显示,不会被渲染

#### Scenario: 嵌套在列表里
- **WHEN** visual 代码块缩进在某个列表项里
- **THEN** 按普通代码块显示源码

### Requirement: 隔离框不能接触 ACECode 与本机
隔离框 SHALL 是只允许脚本、没有同源权限的独立文档,并带固定的内容安全策略:脚本、样式与字体只能来自 cdnjs.cloudflare.com、cdn.jsdelivr.net、unpkg.com、esm.sh,网络请求只能发往这四个域名,图片与媒体只能是 data: / blob:。框内代码 MUST NOT 能读写外层页面、ACECode 的存储与 Cookie,也 MUST NOT 能请求本机 daemon 或任何其它地址;不能提交表单、弹出新窗口或导航外层页面。

#### Scenario: 请求本机 daemon
- **WHEN** 框内脚本执行 `fetch('http://127.0.0.1:<daemon 端口>/api/sessions')`
- **THEN** 请求被内容安全策略拦下,daemon 收不到这次请求

#### Scenario: 访问外层页面
- **WHEN** 框内脚本读取 `window.parent.document` 或 `localStorage` 里的 ACECode 数据
- **THEN** 读取失败,拿不到外层页面与 ACECode 的任何数据

#### Scenario: 从允许的 CDN 加载图表库
- **WHEN** 框内有 `<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.min.js">`
- **THEN** 网络可达时库正常加载,后续脚本可以使用它

### Requirement: 内置图表库
隔离框 SHALL 内置 Chart.js 4 与 d3 7。框内经典脚本的 `src` 指向 cdnjs.cloudflare.com、cdn.jsdelivr.net、unpkg.com 或 d3js.org 上这两个库(Chart.js 主版本为 4 或未写版本,d3 主版本为 7 或未写版本)时,SHALL 改用 ACECode 打包的代码执行,不发网络请求;主版本不同、模块脚本或取不到本地代码时 SHALL 照原地址加载。内置库 MUST NOT 放宽隔离框的内容安全策略,框内代码仍然请求不到本机 daemon。

#### Scenario: 断网画 Chart.js 图表
- **WHEN** 机器连不上外网,模型写了 `<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.min.js">` 与调用 `new Chart(...)` 的脚本
- **THEN** 图表照常画出

#### Scenario: 引用旧版本
- **WHEN** 模型引用 Chart.js 2.9.4 的地址
- **THEN** 按原地址从 CDN 加载,不替换成内置的 4.x(两者配置写法不兼容)

#### Scenario: d3 官网地址
- **WHEN** 模型写 `<script src="https://d3js.org/d3.v7.min.js">`(该域名不在放行名单里)
- **THEN** 使用内置 d3,后续脚本里 `d3.select` 可用

### Requirement: 边写边画
模型还在输出时,未闭合的 visual 代码块 SHALL 随正文增量更新,MUST 复用同一个隔离框、尽量保留已画出的节点,且 MUST NOT 执行其中的脚本。代码块闭合(或消息结束时仍未闭合)后 SHALL 按文档顺序把每个脚本执行一次,外链脚本等加载完成再执行下一个,之后在框内补发一次 `DOMContentLoaded` 与 `load`。

#### Scenario: 流式增长
- **WHEN** 一段 SVG 分十几次追加输出
- **THEN** 隔离框只创建一次,图形逐步出现,已出现的形状不会闪烁重建

#### Scenario: 脚本等闭合后再执行
- **WHEN** 代码块末尾有 `<script>`,模型还没写出结尾的围栏
- **THEN** 脚本不执行;围栏写出后脚本执行一次,注册在 `DOMContentLoaded` 上的初始化代码也会运行

#### Scenario: 消息结束时没闭合
- **WHEN** 回复结束,visual 代码块缺少结尾围栏
- **THEN** 按已有内容做最终渲染并执行脚本

### Requirement: 主题、尺寸与复制
隔离框 SHALL 提供与当前 ACECode 主题同步的颜色、字体变量和 SVG 辅助类,并标明浅色 / 深色模式;主题切换时 MUST 原地更新,不重建内容、不重跑脚本。隔离框高度 SHALL 随内容自动变化,不出现内层滚动条;同一块重新挂载时先用上次的高度。框上 SHALL 有复制按钮,复制的是代码块原文。

#### Scenario: 切换深色模式
- **WHEN** 用户在 visual 显示后把外观切到深色
- **THEN** 框内文字与配色随之变化,用户在框里拖动过的滑块位置不变

#### Scenario: 内容变高
- **WHEN** 框内脚本展开一段内容使高度从 300px 变为 520px
- **THEN** 隔离框随之变为 520px,对话里不出现双层滚动条

#### Scenario: 复制源码
- **WHEN** 用户点击隔离框上的复制按钮
- **THEN** 剪贴板里是该 visual 代码块的原文,并提示已复制

### Requirement: 框内追问与链接
框内 SHALL 提供 `ace.send(text)`(别名 `sendPrompt`)。只有在用户真实点击等操作的当下调用才生效;会话空闲时 SHALL 把文本作为用户消息发出,会话忙时 SHALL 只填进输入框不发送;空文本、非字符串或超过 4000 字符的调用 MUST 忽略,同一隔离框 1 秒内最多生效一次。框内链接点击、`ace.openLink(url)` 与 `window.open` SHALL 只把 http / https / mailto 地址交给系统浏览器打开。

#### Scenario: 点击节点追问
- **WHEN** 会话空闲,用户点击图里绑定了 `ace.send('讲讲缓存层')` 的节点
- **THEN** 发出一条内容为「讲讲缓存层」的用户消息并开始新回合

#### Scenario: 脚本自动调用
- **WHEN** 框内脚本在没有用户操作时循环调用 `ace.send`
- **THEN** 一条消息都不会发出

#### Scenario: 会话正忙
- **WHEN** 回合还在执行,用户点击框内追问按钮
- **THEN** 文本填进输入框,不发送

#### Scenario: 链接
- **WHEN** 用户点击框内 `<a href="https://example.com">`
- **THEN** 用系统浏览器打开该地址,对话页面不跳转;`javascript:` 链接没有任何效果

### Requirement: 可视化图表开关
配置 `inline_visuals.enabled` SHALL 默认开启,写配置时默认值省略。设置 > 常规 SHALL 提供「可视化图表」开关,读写 `GET / PUT /api/config/inline-visuals`(body `{enabled}`,patch 语义,非布尔报 400);保存失败时开关回滚并提示。关闭只影响模型是否被告知可以画 visual,已有的 visual 代码块 MUST 仍正常显示。同机其它 daemon SHALL 在下一次发消息前读到新值。

#### Scenario: 关闭开关
- **WHEN** 用户在设置页关闭「可视化图表」
- **THEN** 发 `PUT {enabled:false}`,下一次请求的系统提示不再包含 visual 部分,历史里的 visual 照常显示

#### Scenario: 另一个工作区
- **WHEN** Desktop 在工作区 A 的设置里关闭开关,随后在工作区 B 的会话里发消息
- **THEN** 工作区 B 的这次请求同样不含 visual 部分

#### Scenario: 非法请求
- **WHEN** `PUT /api/config/inline-visuals` 的 body 是 `{"enabled":"no"}`
- **THEN** 返回 400,配置不变

### Requirement: 画不了图的地方给占位提示
TUI 显示助手消息时 SHALL 把 visual 代码块显示成一行占位提示,不输出源码。IM 消息通道发出助手文本前 SHALL 把每个 visual 代码块替换为一句「请在 ACECode 桌面版或网页中查看」的提示,其余文本(含 mermaid 代码块)MUST 原样保留。

#### Scenario: TUI 恢复网页里的会话
- **WHEN** 在 TUI 里 resume 一个含 visual 代码块的会话
- **THEN** 该位置显示一行可视化内容的提示,不刷出整段 HTML

#### Scenario: 发到 IM
- **WHEN** 绑定了 Telegram 的会话回复了「说明文字 + visual 代码块 + 结论」
- **THEN** Telegram 收到说明文字、一句查看提示和结论,没有 HTML 源码
