# HTML 网页批注精确修改需求规格（重审版）

**日期：** 2026-11-21（2026-11-22 按技术方案裁决对齐：3.2 / 3.3 / 3.4 / 3.5 / 3.7 / 3.8 与第 4、6 章）
**状态：** 需求已确认；与 `docs/specs/2026-11-21-html-page-annotations-technical-design.md`（Q1–Q36 / U1–U5 / D1–D8）一致
**来源：** 工作树现状（`codex/html-page-annotations-194` 的 #194；`codex/html-page-annotations` 集成分支的 #195–201）规格不清晰，重审业务流程后的结论。竞品依据来自 t3code 源码与 Cursor / Codex（ChatGPT）/ Claude Desktop / Pinta / Pinpoint / PigeonDeck / Loupe / Agentation / Drawbridge 的公开实现。第 8 章为针对开放技术决策的竞品事实调研，仅陈述事实，不构成决策依据。

## 1. 目标与问题

Desktop 用户想精确修改 agent 生成的 HTML 页面，现在只能写文字描述让模型猜。要让用户在浏览器里圈出目标，同时表达两类意图：

- **文字批注**：模糊意图（"这里太大了"），模型改完才知道对不对，靠迭代收敛。
- **改值指令**：直接调出目标效果，产生 `property: 旧值 → 新值` 的精确差异，模型照着改，遵从度高。

两类意图属于**同一条批注**，一起写、一起发。

本规格取代 `openspec/changes/add-html-page-annotations/specs/html-page-annotations/spec.md` 作为需求基线；原 spec 描述的是被重审推翻的路线（预览为主、整页截图、不可变记录、收据去重、导航确认）。原 change 已部分实现，代码全在 `codex/html-page-annotations` 分支上（**未进主干**；#194–#201 等分支只是同一份代码的递进快照，内容已被该分支吸收），后续按本规格重新对齐。

## 2. 核心模型

一条批注是一份"意图包"，包含：

| 组成 | 内容 |
|---|---|
| 文字描述 | `comment`，用户写的修改要求 |
| 改值记录 | 每条 `{目标, 属性, 旧值, 新值}`，只记录实际变化 |
| 目标定位 | selector / 组件名 / 源文件行号 / HTML 片段 / 计算样式 |
| 目标类型 | 元素（div / input / button 等）、框选区域、手绘标注 |
| 编号 | 本轮内的展示序号：按创建顺序连续，删除后前移重排、无空档；整批发送清空后下一轮从 1 重新开始 |

页面上的**编号图钉**与聊天输入框里的**批注卡片**是同一份批注的两个视图，始终同步。

## 3. 功能需求

### 3.1 批注创建与双轨输入

- 用户在已加载页面进入批注模式，圈选一个或多个元素、框选区域或手绘标注后创建批注，获得一个编号图钉。编号是本轮内的展示序号，按创建顺序从 1 连续递增。
- 同一条批注内，用户既能写文字描述，也能通过样式面板直接改属性（字体、字号、字重、颜色、背景、透明度、圆角、边框、宽高、间距、padding、margin 等）。
- 改值实时作用于页面 DOM，用户所见即所得；**保存后改后效果留在页面上**。
- 改值只记录实际发生变化的属性：`旧值 → 新值`。原样保留的文本/样式不进记录。
- 元素定位信息随批注一起记录（selector、组件名、源文件行号、HTML 片段、计算样式），与截图一起交给模型。

### 3.2 原图对比

- **按住**查看原图（不是开关）：**只认工具栏按钮的按下/抬起，不设键盘快捷键**（空格、Enter 都不触发）。按住时所有改值效果关闭、编号图钉隐藏；松手恢复改后效果与图钉。
- 对比不改变批注记录本身，只切换预览呈现；松手后图钉留在原位，不重算坐标。
- 「原图」的基准是**所有批注发生前的首次值**，不是上一次修改前的值——多次改同一属性时，按住看到的是页面原始状态。
- 失焦兜底：`blur` / `visibilitychange` / `pointercancel` 任一触发即强制释放，避免卡在"按住"状态。
- 与「隐藏标记」正交：隐藏标记是点击锁存的持久偏好（只隐藏批注层，不还原用户改的值），两者可同时生效。

### 3.3 批注卡片（聊天输入框）

- 聊天输入框上方展示一张待发卡片，聚合**该会话内所有页签**尚未发送的批注，按页签分组（组头是页面标题）。
- 每条显示：按保存时裁剪的缩略图、元素标签（div / input / button …）、文字描述；右侧是铅笔与垃圾桶。
- 卡片最多展示 5 条，超出部分在卡片内部滚动，不把会话挤走；底部有 `N 条注释` 可折叠整卡。
- **点铅笔 = 回到该条继续改**：
  - 页签还在 → 切到该页签、页面回到批注模式、打开该条批注框，描述回填且光标定位在输入框末尾；「调整」（样式面板）可用。
  - 页签已关 → 弹窗只改文字；「调整」不出现（没有页面可比对原始样式），目标定位信息只读。
- **点垃圾桶 = 删除该条**：立即生效，5 秒内可撤销。
- 卡片的编辑/删除与页面图钉同步：删一条 → 对应图钉消失、后续编号前移；改文字 → 卡片与图钉同步。
- 已发送的消息卡片**只读**：卡片主体是该页的**整屏截屏**（单击看大图），下方一行是页面标题与「N 条注释」；指针悬停标签时浮出明细，每条一行「元素标签（div / input / p…）+ 描述」，最多 5 条，超出时列表内部滚动。不提供编辑、删除、复制或"回到页面修改"。

### 3.4 批量发送

- 多条批注可攒成一批一次提交（保留页面编号图钉与待发计数）。
- **消息只在输入框组装一次，触发有三处**：输入框的发送按钮、页内工具栏的发送按钮、浮窗内 `Ctrl+Enter`。三者等价，都是"把输入框里的内容和所有待发批注批量提交到当前会话"；工具栏发送的提交范围是**会话内全部待发批注**（含其他页签与已关闭页签），按钮上显示总条数。
- 输入框里同时有用户自己敲的话时，**合并成一条消息**发出（用户文本 + 空行 + 批注载荷），不拆成两条——拆开会触发两轮对话，模型分两次读同一批批注。
- 发送内容：每条的文字描述 + 精确差异 + 元素定位 + **按目标外接矩形裁剪的截图**（非整页）。截图在**保存批注时**就已截好，发送阶段只做上传，不再捕获。
- 保存时同时留一张**该页整屏图**（每页只保留最新一张），仅用于已发送消息卡片的展示，**不进消息载荷、不发给模型**——模型只收每条的目标裁剪图，避免整屏图挤占上下文。
- 发送走现有会话消息链路，不自建复杂状态机。
- **等确认再清空**：接口返回 202 只代表入队，不算成功；收到这条消息的确认事件后才清空这批图钉与卡片。
- 40 秒仍未收到确认时，先按客户端消息标识核对这条消息是否真的入库：已入库按成功收尾；确实没入库才把批注放回待发、重排编号并提示重试。
- 发送成功后这批图钉清空，下一轮编号从 1 重新开始，不跨轮累计。

### 3.5 导航行为

- 砍掉导航确认弹窗，不拦截用户操作。
- **导航/刷新不作废未发批注**：页面重新加载后，按记录的目标定位在新文档上重放预览，编号沿用原值。
- 重放只写"实时值仍等于原始值"的属性——用户或页面自己改过的值不覆盖，也不猜，直接标为已失效。
- 页签关闭也**不**丢弃未发批注：它们转为"页面已关闭"，仍可改文字、删除、发送（图沿用保存时那张，只是不能再调整样式）。

### 3.7 发送中的状态

- 点发送后批注立刻从页面与卡片上消失（用户看到"发出去了"），进入"发送中"。
- 发送中的批注不参与页面渲染、不计入待发数、不可编辑。
- 确认到达即彻底丢弃；失败则回到待发集合末尾、重排编号、恢复图钉，并提示可重试。
- 截图上传失败不阻塞发送：该条无图，消息照发。

### 3.6 编号规则

- 图钉编号是**本轮内的展示序号**，不是持久身份：按创建顺序从 1 连续递增。
- 删除一条批注后，其后所有批注的编号前移重排，始终保持 1..N 连续、无空档。
- 整批发送成功清空后，下一轮的第一条批注编号重新从 1 开始，不与上一轮累计。
- 编号仅在单批消息内用于指代"第几条修改"，不跨批引用。

## 4. 明确不做（非目标）

| 项 | 说明 |
|---|---|
| 整页截图 | 改为按批注目标裁剪；不做视口外全页捕获。视口内的整屏图只用于消息卡片展示，不进消息载荷、不发给模型 |
| 逐条状态机 | 只有"发送中"一个中间态；不做 pending/sending/sent 三态消费方 |
| 接收端收据去重 | 不改接收端；走现有消息投递语义 |
| 导航确认弹窗 | 不拦截、不清空；改为重放 |
| 已存记录不可编辑/不可删除 | 发送前可编辑、可删除（卡片和图钉同步）；发送后只读 |
| 重启持久化 | 仅当前 Desktop 运行期保留，重启清空，不写磁盘 |
| 自建发送/重试通道 | 复用现有会话消息链路 |
| 编辑已发送的消息 | 不做。消息是对话记录，模型已按当时描述执行过；改历史会让对话与执行结果对不上 |

已知取舍：发送失败后的重发**先自查**（按客户端消息标识核对是否已入库），但仍不保证模型端去重——极端情况下（自查也失败）可能重复执行同一修改。这是砍掉收据机制后明确接受的代价。

## 5. 安全与兼容约束

- 批注桥接只在可信 WebUI 侧注册，网页侧不可访问；网页 DOM 与特权发送严格隔离。
- 页面提供的任意消息不得当作特权命令。
- 保留现有"将元素添加到聊天"（source selection）入口，两种模式不共用事件或草稿。
- 遵循仓库 UI 文案强约束：只保留完成操作所需的标题、标签、内容、操作，不加冗余说明。

## 6. 验收标准

1. 用户圈选元素后可同时写文字并改多个属性，保存后页面保持改后效果，图钉编号按创建顺序从 1 连续递增。
2. 按住"看原图"时所有改值效果关闭、图钉隐藏；松手后改后效果与图钉完整恢复，切走窗口失焦也能释放；全程用键盘不触发该效果。
3. 聊天框待发卡片显示正确 N 条，每个页签一组；每行可见缩略图、元素标签与描述；超过 5 条时卡片内部滚动。
4. 在卡片中编辑某条，对应批注框弹出且输入框聚焦；修改后页面图钉同步更新。
5. 在卡片中删除某条，页面对应图钉同步消失，剩余批注的编号前移重排、保持 1..N 连续无空档。
6. 多条批注一次发送，模型收到每条的文字 + `属性: 旧值 → 新值` 精确差异 + 元素定位 + 裁剪截图；无整页截图。整屏图只出现在 UI 卡片上，不出现在消息载荷里。
7. 收到发送确认后这批图钉清空、待发计数归零；下一轮创建的第一条批注编号重新从 1 开始。40 秒无确认但消息实际已入库时，不得回滚、不得产生重复消息。
8. 页面刷新/导航后未发批注仍保留，预览按记录重放到新文档上，编号沿用原值；重启 Desktop 后清空。
9. 网页侧无法调用批注桥接或触发会话发送。
10. 现有"将元素添加到聊天"入口行为不变，与批注互不干扰。
11. 关闭某个页签后，该页的未发批注仍出现在卡片上（标注"页面已关闭"），可改文字、可删除、可发送，且消息里仍带保存时那张截图。
12. 输入框里有文字且有批注时点发送，只产生一条消息；已发送的消息卡片是整屏截屏 + 「N 条注释」，卡片上没有任何编辑入口，单击截屏可看大图。
13. 悬停「N 条注释」浮出明细，每条一行「元素标签 + 描述」；7 条时浮层显示 5 行、可内部滚动。

## 7. 竞品调研依据

| 决策 | 竞品事实 |
|---|---|
| 双轨合一（文字 + 改值） | Cursor Design Mode 的 note 可同时带文字、CSS 属性编辑、语音；Pinta "captures the diff as a structured CSS payload"；PigeonDeck 导出"CSS 选择器和变更对照表" |
| 图钉 + 批量 | Cursor "Batch edits on → notes collect as numbered pins, Send button with count sends all as one message"；Drawbridge 有 to do/doing/done 任务状态 |
| 裁剪截图 | t3code 按目标外接矩形裁剪；Pinpoint "cropped screenshot"；Agentation "screenshots of surrounding elements"；Loupe 是少数整页派 |
| 无三态/收据 | Cursor 发送即清空；t3code 一次提交一张 chip；Pinpoint 即时对话（可反问、before/after、Accept）；均无自建幂等去重 |
| 卡片入口 + 可编辑删除 | Cursor "Open the list next to Send to review pending notes, or remove them all with Clear All"，note 发送前可改；Pinpoint 对话式修改；t3code chip 可编辑可删 |
| 导航不清空无弹窗 | t3code 导航直接取消当前批注；PigeonDeck 会话级清空；Pinpoint 用持久化 pins 而非弹窗 |
| 运行期清空 | PigeonDeck "所有数据仅存在于当前标签页会话，关闭标签即清除" |
| 原图对比（本次保留，竞品少有） | Cursor 用 Cmd+Z 撤销 markup、Esc 还原文本编辑；t3code 退出即还原；PigeonDeck 有撤销/重做但无全局对比开关。此为本项目明确差异点 |
| 编号重排、清空后重置 | WariPixel "deleting one renumbers the rest — no gaps"；CobaltCapture "Delete one and the rest renumber themselves"；Maxisnap "Delete a middle pin and the rest re-sequence"；shadcn/Calloutly "removing a pin renumbers the rest"。展示序号是位置序号，非持久身份；Annotaitr 用 UUID 派生稳定 handle 与位置序号分离 |

## 8. 开放技术决策的竞品事实调研

本章针对第 9 章的 9 个开放技术决策，逐条陈述竞品的公开做法。**仅陈述可核实的事实，不构成决策依据，不含推荐或取舍判断。** 事实来源分三类：官方产品文档 / API 文档、公开源码、第三方扩展与社区文档，表中逐条标注。需特别说明：Anthropic 官方（Claude in Chrome / Claude Desktop 内置浏览器）公开文档未记录元素级页面批注能力，Claude 生态的批注流程由第三方 MCP 扩展提供；Codex / ChatGPT 则有官方 Annotate、Adjust 与 Browser Annotation API。

### 8.1 状态与双视图同步（对应决策 1）

| 竞品 | 公开事实 | 来源 |
|---|---|---|
| Cursor Design Mode | Batch edits 开启时，pending notes 在页面上以 numbered pins 呈现；Send 按钮旁的 list 可查看 pending notes，Clear All 清空全部；已发送 notes 不能从队列清除；带 pending notes 退出 Design Mode 时 Send 按钮保留在工具栏，直到发送或清除。文档未公开 pin 的稳定身份字段。 | Cursor 官方 Design Mode 文档 |
| Codex / ChatGPT | Browser Annotation API 的 `request()` 返回 `accepted`，仅表示请求被浏览器接收并校验，不代表编辑器已打开、已保存或已发送；保存的 annotations 可在 composer attachment preview 中重开、编辑；退出 Annotation mode 关闭编辑器并保留 saved annotations，不自动提交、不移动到 composer。Canvas 虚拟对象要求 `id` 在 surface 内稳定，控件事件携带 `surfaceId` + `targetId`。 | ChatGPT 官方 Browser Annotation API 文档、Help Center |
| t3code | 一条 annotation 带内部 id（`element_<pageload>_<seq>`）；提交后进 composer draft 成为一张 chip；chip 在发送前可编辑、可删除。 | 源码 `AnnotationIds.ts`、`PickPreload.ts`、`composerContextRecords.ts` |
| Vibe Annotations | annotations 存于本机 `~/.vibe-annotations/annotations.json`，每条带 annotation ID；Claude Code 插件把新 annotations 汇总显示在 prompt 上方并提供 Implement 按钮；agent 实现后通过 `delete_annotation` 工具删除已完成项。 | 扩展商店页、MCP 文档 |
| Loupe | 每条 annotation bundle 写为 `~/.loupe/annotations/` 下独立 JSON 文件，可通过 MCP 寻址；键盘 `1-9` 选择编号 pin，`Del` 删除选中 pin。 | Loupe 官方 manifesto、扩展说明 |

### 8.2 原图对比与改值留存（对应决策 2）

| 竞品 | 公开事实 | 来源 |
|---|---|---|
| Codex / ChatGPT | Adjust 预览通过 `oaiannotationcontrolchange` 事件传递三种 action：`preview`（显示请求的变化）、`preview-original`（临时显示原始状态用于比较）、`reset`（清除预览时恢复原始状态）；文档要求 handler 可逆且可安全重复调用，并要求把普通草稿、未完成输入与 preview 状态分开，避免无关编辑覆盖预览；preview 本身不构成永久修改，永久保存走应用正常 save flow；`dispose()` 只移除控件注册，不撤销 preview 变化，移除集成时需手动恢复渲染状态。Help Center 原文："A preview helps communicate the change you want. It does not, by itself, save a permanent change to the website." | ChatGPT 官方 Browser Annotation API 文档、Help Center |
| Cursor | 双击文本可就地编辑，Enter 保存、Esc 还原；Cmd/Ctrl+Z 撤销 markup；CSS inspector 改值随 note 发送，官方未描述实时 DOM 写入与还原细节。 | Cursor 官方 Design Mode 文档 |
| t3code | `setStyleForSelected()` 以 `!important` 写入真实元素 inline style，同时记录 computed style 旧值到新值；teardown 时 `restoreStyles()` 按 baselineStyles 全部还原。 | 源码 `PickPreload.ts` |
| Vibe Annotations | 提供 live preview 的 CSS 属性调整（font size / colors / layout / spacing），变化被捕获为 annotations，交给 agent 在源码中实现。 | 扩展商店页 |
| Loupe | 可在页面拖拽手柄 resize、就地编辑文案、换色、删除 section、调整兄弟节点顺序，改动持续到 pin 提交。 | Loupe 官方 manifesto |

### 8.3 元素定位信息获取（对应决策 3）

| 竞品 | 公开事实 | 来源 |
|---|---|---|
| Cursor | 每个元素提供 tag、id、classes、attributes、CSS 选择器路径、前 200 字可见文本、尺寸与位置，以及 6 项 computed styles（color / background-color / font-size / font-family / display / position）；React / Vue / Svelte 应用附带 component name 与 component stack；文档明确不承诺 full DOM dump、完整样式表、全部祖先 computed styles、完整应用状态或全页截图。 | Cursor 官方 Design Mode 文档 |
| t3code | `getElementContext` 提供 selector、componentName、source（file / line / column）、htmlPreview、styles；5 秒超时，失败回退到 outerHTML 前 500 字。 | 源码 `PickPreload.ts` |
| Vibe Annotations | 捕获 CSS selectors、styles、positioning、viewport，以及从页面 fiber tree 读取的 React / Vue component 与其源文件。 | 扩展商店页 |
| Loupe | 每条 bundle 含 CSS selector、20 项 computed styles、父级 HTML、会话期间 console errors、URL、viewport、语言、active framework、breakpoint。 | Loupe 官方 manifesto、扩展说明 |
| Codex / ChatGPT | 网站可通过 `oai-annotation-metadata` 附加结构化上下文（最多 6 个属性、序列化后不超过 2048 bytes），随 annotation 进入对话；Canvas 对象可带 name / role / metadata / rect；文档要求只附加愿意同时展示给用户和模型的信息；内置浏览器把页面内容视为 untrusted。 | ChatGPT 官方 Browser Annotation API 文档、Help Center |
| Inspector Jake | `inspector_get_selected_element` 返回 tagName / id / className / selector / innerText / attributes / rect；`inspector_snapshot` 返回 ARIA 可访问性树；`inspector_get_page_info` 返回 URL / title / viewport。 | 第三方 MCP 扩展文档 |

### 8.4 裁剪截图生成（对应决策 4）

| 竞品 | 公开事实 | 来源 |
|---|---|---|
| Cursor | 保存 note 时把截图裁剪到所选元素，或裁剪到带 markup 的框选区域；文档不承诺全页截图。 | Cursor 官方 Design Mode 文档 |
| t3code | 先 `capturePageWithRetry` 截全页，再 `cropAnnotationScreenshot` 按 CSS 坐标乘以 devicePixelRatio 裁剪，裁剪矩形为所有目标外接矩形并集加 20px padding，忽略滚动出视口的目标；截图失败降级为纯文字批注并 toast。 | 源码 `Manager.ts`、`PreviewView.tsx` |
| Loupe | 生成滚动拼接的全页标注截图（pin 烧录其上），同时为每条 annotation 生成聚焦裁剪图。 | Loupe 官方 manifesto、扩展说明 |
| Codex / ChatGPT | `request()` 的 `omitScreenshot` 可省略自动截图；截图作为 composer attachment；官方测试指南要求验证移除附件时同步清除 preview。 | ChatGPT 官方 Browser Annotation API 文档 |
| Pinpoint / Agentation | Pinpoint 为 cropped screenshot；Agentation 为 screenshots of surrounding elements（第 7 章已记）。 | 第 7 章调研记录 |

### 8.5 改值指令格式（对应决策 5）

| 竞品 | 公开事实 | 来源 |
|---|---|---|
| t3code | 文本投影为 `requested visual changes:` 下的 `- property: prev → value` 列表，同时保留 `styleChangeDetails` 完整结构用于复制 / 粘贴还原。 | 源码 `composerContextReferences.ts`、`composerContextRecords.ts` |
| Codex / ChatGPT | 自定义控件记录 label、reference、old value、new value、callback；select 控件发送给模型的是可见 option label 而非内部 value；metadata 与 requested values 随注释进入对话。 | ChatGPT 官方 Browser Annotation API 文档 |
| Cursor | 官方描述为 "Cursor sends your CSS edits with the note"，未公开具体文本格式。 | Cursor 官方 Design Mode 文档 |
| PigeonDeck | 导出为 CSS 选择器与变更对照表（第 7 章已记）。 | 第 7 章调研记录 |

### 8.6 批注卡片与 composer 集成（对应决策 6）

| 竞品 | 公开事实 | 来源 |
|---|---|---|
| Codex / ChatGPT | 保存的 annotations 以 composer attachment preview 形式存在，可重开、编辑；发送时随消息一起提交；`request()` 只打开编辑器，不发送消息。 | ChatGPT 官方 Browser Annotation API 文档、Help Center |
| Cursor | Cmd/Ctrl+L 把 note 或 text edit 的内容放进 chat input 而非发送；Option/Alt+click 元素直接加入 chat input；Batch edits 下工具栏 Send 按钮显示 pending 计数。 | Cursor 官方 Design Mode 文档 |
| t3code | Enter = attach（批注进入 composer draft 成为 chip），Cmd/Ctrl+Enter = send；chip 可编辑、可删除。 | 源码 `AnnotationKeyboard.ts`、`PickPreload.ts` |
| Vibe Annotations | Claude Code 插件把新 annotations 显示在 prompt 上方并以一个请求发送；`/vibe list` 打开列表，`/vibe auto on/off` 切换自动发送。 | MCP 文档 |
| Inspector Jake | 一键打开 chat 并把所有 capture（含 selector、styles 与 note）发送到 agent 会话。 | 第三方 MCP 扩展文档 |

### 8.7 批量发送提交语义（对应决策 7）

| 竞品 | 公开事实 | 来源 |
|---|---|---|
| Cursor | Batch edits 开启时一次 Send 把所有 pending notes 作为一条消息发送并退出 Design Mode；已发送 notes 不能从队列清除；官方文档说明批量不保证模型正确实现每条指令。 | Cursor 官方 Design Mode 文档 |
| Codex / ChatGPT | 只有用户可以提交 annotation；`accepted` 仅表示请求被接收校验；退出 Annotation mode 不自动提交。 | ChatGPT 官方 Browser Annotation API 文档 |
| t3code | submission 取 attach 或 send 两种，即时提交；一次批注对应一条 chip，无独立待发池；接收端无收据去重机制。 | 源码 `PickPreload.ts`、`Manager.ts` |
| Vibe Annotations | agent 读取并实现后通过 `delete_annotation` 删除已完成 annotation；另有 watch mode 长轮询新 annotation。 | 扩展商店页、MCP 文档 |
| Drawbridge | 任务状态以 to do / doing / done 写入任务文件（第 7 章已记）。 | 第 7 章调研记录 |

### 8.8 导航清空触发面（对应决策 8）

| 竞品 | 公开事实 | 来源 |
|---|---|---|
| t3code | 监听 `did-start-navigation`（main frame）时 `settle(null)` 丢弃进行中的 pick；tab 切换 / 组件 unmount 也取消；Esc 取消当前 overlay。 | 源码 `Manager.ts`、`PreviewView.tsx` |
| Cursor | dev server hot reload 时 agent 的改动出现在运行页面；带 pending notes 退出 Design Mode 时 Send 按钮保留，而非清空。 | Cursor 官方 Design Mode 文档 |
| Codex / ChatGPT | 退出 Annotation mode 关闭编辑器并保留 saved annotations，不自动提交、不移动到 composer。 | ChatGPT 官方 Browser Annotation API 文档 |
| Vibe Annotations / Loupe | annotations 写入本机磁盘（`~/.vibe-annotations/`、`~/.loupe/annotations/`），跨标签页与导航保留；Vibe 明确支持多路由的 multi-page annotations。 | 扩展商店页、Loupe 官方 manifesto |

### 8.9 测试与验收（对应决策 9）

| 竞品 | 公开事实 | 来源 |
|---|---|---|
| Codex / ChatGPT | Browser Annotation API 文档给出测试项：用户可保存、重开、编辑 composer attachment preview 中的 annotation；发送后 ChatGPT 收到选中内容、metadata 与 requested values；移除附件时清除 preview；编辑器加载期间需检查 `request().accepted`，同期最多持有一个 accepted request。 | ChatGPT 官方 Browser Annotation API 文档 |
| Cursor | 公开文档以快捷键表、上下文清单与工作流说明为主，未公开自动化测试或双平台真机验收流程。 | Cursor 官方 Design Mode 文档 |
| t3code / Vibe Annotations / Loupe / Inspector Jake | 公开文档描述功能与本地运行方式，未公开针对批注流程的自动化测试覆盖范围，也未公开 Windows / macOS 双平台真机验收清单。 | 各项目公开文档与源码 |

## 9. 开放技术决策（交给 grilling）

以下决策项不在本章给出结论；第 8 章的竞品事实仅作参考信息，不作为决策依据。

1. **批注状态与视图同步机制**：页面图钉和聊天卡片是同一份批注的两个视图，如何保证编辑/删除双向同步？状态放在哪一侧（可信 WebUI / 原生宿主）？编号是可重排的展示序号，同步/寻址所需的稳定身份如何单独承载（如内部 ID），避免与编号混为一谈？
2. **原图对比层与改值留存的共存**：对比开关需要独立样式层 + 文本层做可逆切换，而"改后效果留页面 + 图钉同步"要求状态持久。两层机制如何组织，切换时如何保证恢复无残留（尤其多次编辑同一目标、文本子节点保真）？
3. **元素定位信息的获取**：selector / 组件名 / 源文件行号 / HTML 片段 / 计算样式分别从什么边界取，如何在不可信页面与可信宿主间安全传递？与现有 `selected_element` 的源码定位是否共用？
4. **裁剪截图的生成**：按目标外接矩形裁剪的捕获边界在哪（Windows WebView2 / macOS WKWebView）？跨元素、跨区域如何取外接矩形？捕获失败时的降级行为？
5. **改值指令的格式**：`{目标, 属性, 旧值, 新值}` 如何编码进现有会话消息，模型端如何解读"照着改"？是否复用 t3code 的 `requested visual changes: - property: old → new` 文本格式？
6. **批注卡片与现有 composer 的集成**：卡片挂在哪个组件，如何与草稿、附件、队列/steer 语义共存？编辑时弹出批注框的焦点/键盘处理？
7. **批量发送的提交语义**："发送即清空"是发送前清空还是收到确认后清空？失败时如何整批重发（接受无去重风险）？与现有会话发送链路的 ack 如何衔接？
8. **导航清空的触发面**：哪些入口算"导航/刷新"（页面内跳转、host 主动跳转、刷新、关闭标签）？各自如何触发清空？
9. **测试与验收**：行为自动化覆盖到哪一层（前端交互 / 原生模型 / 端到端）？Windows/macOS 真实桌面验收的范围与未验证项如何记录？
