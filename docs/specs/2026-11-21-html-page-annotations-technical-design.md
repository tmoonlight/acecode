# HTML 网页批注 —— 技术方案（v2）

- 状态：待评审（决策已清零：Q/U 见 §1，细节 D1–D8 见 §1.1）
- 日期：2026-11-21
- 基线：worktree `C:/Users/Administrator/.codex/worktrees/html-page-annotations/acecode`（分支 `codex/html-page-annotations`，`bc10d7f0` + 未提交 WIP）
- 取代：`openspec/changes/add-html-page-annotations/`（设计路线：预览为主 / 整页截图 / 不可变记录 / 收据去重 / 导航确认）
- 需求：`docs/specs/2026-11-21-html-page-annotations-rewrite-requirements.md`

---

## 0. 一句话方案

页面侧的批注运行时（样式层 / 文本层 / 区域框选）保留，**记录权威上提到会话级 `SessionAnnotationStore`**，页签降级为渲染/编辑端点（关了也不丢记录）；待发批注以一张卡片挂在会话输入框上方，**逐条可编辑、可删除**；**消息组装只有一处（composer），触发点三处**（输入框发送、页内工具栏发送、浮窗 `Ctrl+Enter`），三处调同一个提交动作，发出即一条自包含消息（用户文本 + `Page annotations` + JSON，无二次确认）；「按住看原图」「隐藏标记」「刷新后重放」「源文件定位」按本文档的约束补齐。

---

## 1. 决策记录

保留拷问过程里的结论，作为后续变更的对照基线。

| # | 问题 | 裁决 |
|---|---|---|
| Q1 | 原图对比是开关还是按住 | **按住**。纯同步，只增删 class，禁止重渲染/截图实现 |
| Q2 | 按住什么键 | **只有工具栏按钮的按下/抬起（纯指针）**，不设键盘快捷键；`pointercancel`/`pointerleave` 兜底 |
| Q3 | 原图与隐藏标记的关系 | **两个独立控件**，投影正交 |
| Q4 | 「原图」的基准 | **所有批注发生前**（首次触碰值），不是上一次修改前 |
| Q5 | 文本编辑 | 在范围内（#196 已实现，逐 Text 节点写 `node.data`） |
| Q7 | 隐藏标记隐藏到什么程度 | **整层隐藏**（图钉、描边、选中框、hover 卡全不见） |
| Q8 | 原图态图钉 | **不显示**；松手后图钉留在原位（不需要重算 rect） |
| Q9 | 隐藏标记是按住还是锁存 | **点击锁存**（持久视觉偏好） |
| Q10 | 按住时样式面板 | **整体隐藏**（视觉隐藏 + `inert`，不卸载） |
| Q14 | 按住是否与轮询排队 | **不排**。preview 类命令是 quiet 命令，不占 `busy_` |
| Q15 | 编号与记录权威 | **原生权威**；页内只接收 `sync` 全量下发的图钉列表 |
| Q16 | 隐藏标记交互 | 保留点击锁存 |
| Q17 | 失焦兜底 | **补**：`window blur` / `visibilitychange` / `pointercancel` 强制释放 |
| Q18 | 浮窗打开时工具栏 | **消失**（沿用 Codex 行为，规避丢草稿与挖洞几何抖动） |
| Q19 | 隐藏标记范围 | 改实现为整层 `visibility:hidden` |
| Q20 | `old_value` 语义 | **真 baseline**（首次触碰值），中间态不进记录 |
| Q21 | 失效批注能否发送 | **能**，标 `target_invalid` |
| Q22 | 清空点 | 只剩两个：**消息确认** + 手动丢弃。（导航/刷新/重启不清） |
| Q23 | URL 粒度 | **每条记录自带** `page_url` / `page_title` |
| Q24 | 清空时机 | **等 WS `message` 事件按 `client_message_id` 确认**，不是 202 |
| Q25/Q28 | 截图 | **保存批注时按 rect 裁剪**，每条一张，供待发卡片缩略图；同一时刻另存该页**整屏图**给已发消息卡片，每页只留最新一张。发送时**复用不再捕获**（§4.8）。两张图用途不同，不是同一张 |
| Q26 | 批次回执 | **删干净**（`annotation_batch_id`、`reconcileOnly`、服务端对账分支） |
| Q27 | 失效与有效混批 | **全部放行**，逐条标状态 |
| Q29 | 源文件定位 | **零注入**：主 world 读 `__reactFiber$`，dev 给 file:line，否则给组件栈；定位不到必须问 |
| Q30 | 刷新后保留 | **要**：声明式重放（selector + 差异），带「值相等」闸 |
| Q31 | mac 视口外目标截图 | 接受降级（保存时即无图），UI 明示 |
| Q32 | 重放闪烁 | 接受（与 Codex 同级） |
| Q33 | 记录归属 | **修订为会话**：记录归属会话的待发集合，页签只是渲染/编辑端点；**关页签不销毁**，降级为 `orphan`（§4.11） |
| Q34 | 半打字描述 | 内存暂存，**key 用 `page_id + target_locator`**（不含 `document_id`，理由见 §4.10） |
| Q35 | 「发送」的语义 | **直发，无二次确认**。**组装唯一、触发三处**：消息只在 composer 组装一次；输入框发送按钮 / 页内工具栏发送按钮 / 浮窗 `Ctrl+Enter` 都调同一个提交动作，效果等价于「把输入框内容和所有批注批量提交到当前会话」 |
| Q36 | 浮窗 `Enter` | **添加**（保存当前批注、生成图钉等待发送），不发送；`Shift+Enter` 换行 |
| U1 | 已发卡片的落点 | **消息历史**；否掉「发送后仍挂在输入框等待确认」方案。注意这与 U3 的**待发卡片**是两回事：发送前才在输入框上方有一张待发卡 |
| U2 | `reapply` 时机 | **立即 + 观察窗口**（3s / 节流 300ms / 最多 10 次）；否掉「延迟到首次用户操作」 |
| U3 | 待发批注卡片 | 逐条 **编辑 + 删除**。编辑 → 激活页签 + 打开该条浮窗 + 描述框聚焦；**页签已关 → 弹窗只改文本，「调整」隐藏**。已发消息卡片**只读**（§4.11） |
| U4 | `Ctrl+Enter` | **保存当前 + 发送整批**（a）；发送走 composer 同一条链路 |
| U5 | 40s 未确认 | **先按 `client_message_id` 自查再回滚**：已落库 → 正常收尾；未落库 → 才回滚 |

额外裁决（本轮补充）：

- 会话归档**不关闭**其浏览器页签 → 页签与批注保留，重新打开会话可继续。
- **待发批注卡片挂在会话输入框上方**，跨页签聚合（数据源见 §4.11）；发送目标 = 卡片所属会话，不提供会话选择器。
- **已发消息卡片只读**：整屏截屏 + 「页面标题 + `N 条注释`」，明细收进悬停；不给「回到页面修改」「复制」「重发」。批注能改的前提是尚未发送进会话。
- 页签关闭 ≠ 记录消失：记录降级为 `orphan`，仍可改文本、删除、发送；**图还在**（保存时截的），只是不可调整、不可重放。
- 消息载荷**自包含**：移入 `in_flight` 时即生成副本，发送过程中页签关闭不影响发送。

### 1.1 细节裁决（D1–D8，全部已定）

| # | 问题 | 裁决 | 影响 |
|---|---|---|---|
| D1 | `orphan` 记录在重开同 URL 页签时是否「重新定位」回 `live` | **不做**。`orphan` 是终态，只能改文本 / 删除 / 发送 | 关页签后预览不可恢复；agent 仍可按 locator 找元素。留 action 位，后置可加 |
| D2 | 待发卡片每行的 24px 缩略图从哪来 | **保存批注时按 rect 裁剪的那张图**（只给待发卡片）；另存一张**该页整屏图**给已发消息卡片，每页只留最新一张 | 捕获时机从「发送时」前移到「每次 save」；发送时不再捕获。**`orphan` 记录因此仍有图**。删除批注不重截，配图保留删除前画面 |
| D3 | 卡片最多展示几条 | **5 条**，超出后卡片内部滚动 | 不是折叠成 chip，而是 `max-height` + `overflow:auto` |
| D4 | 删除后撤销窗口 | **5s** | 定时器，可调 |
| D5 | 跨页签混批的载荷形态 | **`page` 单值（第一条记录所在页）+ 每条自带 `page_url`** | 解析层零破坏；模型与卡片按 `annotations[].page_url` 分组 |
| D6 | 已发卡片要不要「复制文本」 | **不给**。已发卡片一个按钮都没有 | 与 U3 一致：只读到极致，避免任何"能改"的暗示 |
| D7 | 编辑中页签被关闭 | **存进 draftStore，卡片行转 `orphan`**，下次点铅笔接着改 | 不打断输入、不弹窗、不假装页面还在 |
| D8 | 编辑态要不要把页面拉进批注模式 | **要拉回**：隐式 `start()`，页面进入批注模式 | 用户从卡片点铅笔本就是来改批注的，之后的点击自然就是批注意图；`start` 复位 `original`/`markers_hidden` 也符合"重新进入"的心理预期 |
| D9 | 记录标识是否与文档绑定（#208 结论） | **不绑定**。`record.id` = `<page_id>:annotation:<n>`，`document_id` 降为记录属性，另存跨文档可用的 `locator` | 现行 `<document_id>:annotation:<n>` 在刷新后结构性失效（§4.12）；#209 的存储主键照此落 |
| D10 | 保存时捕获是同步还是异步（#208 结论） | **异步补图**：`save` 立即入库广播，捕获 + 裁剪在后台完成后回写 `thumbnail` / `page_shot`；2s 超时判 `screenshot_unavailable` | 页面侧裁剪实测 P95 ≤ 37 ms（做了 `scale=1/dpr`），性能不是瓶颈；异步是为了不让直接操作动作等 CDP 往返，也为了让捕获失败不阻塞保存 |

---

## 2. 领域模型

```
Session (会话)
  ├── SessionAnnotationStore (原生, 内存)          ← 待发记录的权威
  │     ├── records[]          每条自带 page_id / page_url / page_title
  │     ├── in_flight[]
  │     └── next_number        按 page_id 分组各自递增
  └── Page (浏览器页签)                            ← 渲染与编辑的端点
        ├── AgentBrowserState
        └── PageAnnotationSession (原生, 内存)     ← 页签存活时才存在
              ├── active / original / markers_hidden / modal_suspended
              ├── selection_mode: null | element | region
              ├── targets[] / region
              └── document_id / epoch              ← 只负责选区、预览、样式层
```

**为什么权威从 Page 上提到 Session**：U3 要求「页签关了，卡片还在，还能改文本、还能发送」。只要记录的生命周期短于卡片，`pages.erase(page_id)` 就会把用户写到一半的描述一起析构。反过来把记录挂在会话上，页签只剩三件可失效的东西：DOM（预览/样式/调整）、`document_id`（重放）、`executionContextId`（下发命令）。这三样都在页签关闭时失效，正好对应 `orphan` 态。

**核心不变量**

1. 记录属于 **Session 的待发集合**，不属于页签。关页签 → 记录转 `orphan`，**不删除**。
2. 导航（含刷新、前进后退、跳转）**不作废记录**，只把它从 `live` 转成 `replayable`。
3. 记录编号是**页内展示序号**，等于该 `page_id` 分组内位置 + 1；删除即重排。跨页签混批时编号会重复，靠 `page_url` 消歧（Q23）。
4. 差异（`differences`）的 `old_value` 永远是该属性的**页面原始值**，与「按住看原图」看到的一致。
5. 已发送的消息**不可变**：没有任何入口能改写它（无「编辑消息」API，transcript 只追加）。

---

## 3. 状态机

### 3.1 批注模式（每个 Page 独立）

```
idle
 └─(start)─> annotating
               ├─(pointer 单击元素)─> popup_open(target)
               │                        ├─(编辑样式/文本)─> popup_open(dirty)
               │                        └─(save)─> annotating(records+1)
               ├─(add_region + drag)─> popup_open(region)
               ├─(open_record_popup)─> popup_open(edit)   // 卡片铅笔，回填已有记录
               ├─(exit)─> idle          // 记录保留，工具栏隐藏
               └─(discard)─> idle        // 记录清空，next_number=1
```

- `exit` / `start` 不销毁记录（Q22）。
- `start` 复位 `original` / `markers_hidden` / `modal_suspended`。

### 3.2 记录四态（替代当前 `valid: bool`）

| 态 | 含义 | 可发送 | 画图钉 | 可重放 | 可调整样式 |
|---|---|---|---|---|---|
| `live` | 目标仍在当前文档且有效 | 是 | 是 | — | 是 |
| `replayable` | 文档已替换，等待在新文档上重放 | 是（标 `target_invalid`） | 否 | 是 | 否（等重放成功） |
| `stale` | 重放失败 / 目标值已被改动 / 目标被移除 | 是（标 `target_invalid`） | 否 | 否 | 否 |
| `orphan` | **页签已关闭**，没有任何 DOM 可操作 | 是（标 `target_invalid`；图沿用保存时截的） | 否 | 否 | 否 |

转换：

```
live --(document replaced)--> replayable
replayable --(定位成功 且 实时值==old_value)--> live
replayable --(定位失败 / 值不等 / 文本节点结构变化)--> stale
任意 --(页签关闭)--> orphan        // 记录保留，仍可改文本/删/发
orphan --(重开同 URL 页签)--> 仍为 orphan（不自动重放，需用户显式"重新定位"）
任意 --(点发送)--> 移出 records, 进 in_flight（不再画图钉，见 §3.4）
in_flight --(WS 确认)--> 删除
in_flight --(失败)--> 并回 records 尾部，恢复原态
任意 --(手动删除单条)--> 删除
```

`next_number` 只在 `discard`（全清）时重置为 1；`replace_document` **不重置**，但重排时按该 `page_id` 分组内的数组顺序重新编号（见 §4.2）。`orphan` 记录不参与页内 `sync`（它没有页签），但**仍出现在待发卡片上**。

### 3.3 拦截式预览（按住原图）

```
released --(pointerdown)--> held
held --(pointerup / pointercancel / pointerleave / window.blur / visibilitychange hidden)--> released
```

- **纯指针，零键盘。** 不注册 `keydown`/`keyup`，空格与 Enter 在工具栏上不触发「按住原图」（Enter 对聚焦按钮的默认激活行为要 `preventDefault` 掉，否则会变成一次点击——而点击对「按住」没有语义）。
- 进入 `held` 只做一件事：`original{enabled:true}`。
- 退出 `held`：`original{enabled:false}`。
- `held` 期间 `preview_style` / `preview_text` / `save` 在页内短路返回 `original_view_active`（已实现），原生侧也要拒绝，避免英文错误码外泄。
- 强制释放的优先级高于「用户主动松手」：重复发 `enabled:false` 是幂等的。

### 3.4 批次与发送

**组装唯一，触发三处。** 消息只在 composer 组装一次（`submitWithAnnotations()`），三处都调它：

| 触发点 | 语义 |
|---|---|
| 会话输入框的发送按钮 | 原生入口 |
| 页内工具栏的发送按钮 | 等价于点输入框发送：**输入框现有内容 + 会话内全部待发批注**一起提交 |
| 浮窗 `Ctrl+Enter` | 先 `save` 当前这条，再触发同一个提交 |

工具栏按钮不是第二条发送链路——它不自己组装 payload、不自己 POST，只在原生侧确认 webview 存活后把「提交」这件事交给 WebUI 的同一个动作。这样「输入框里有半句话 + 3 条批注」无论从哪触发，都只产生一条消息、只触发一轮 turn。

三点约束：

- **提交范围是会话全量**，不是「当前页签那几条」。工具栏挂在页签 A 上，点它也会把页签 B 与 `orphan` 的记录一起发出去——因为待发集合本身就是会话级的（Q33）。按钮上必须显示总条数（`N 条待发`），否则用户会以为只发现在这页。
- **输入框草稿会一并发出**。用户可能正在输入框里写话，随后回页面点工具栏发送——那半句话会作为消息前缀一起提交。这是有意为之（用户要的就是「把聊天框里的内容和所有批注批量提交」），不做「工具栏发送时丢弃草稿」的分支。
- **回执反馈收敛到一处**：成功/失败/40s 自查结果统一由 composer 的发送状态机广播，工具栏与输入框显示同一个状态，不各写一份 toast。

```
pending（会话内全部 live / replayable / stale / orphan 记录）
 └─(任一触发点 → submitWithAnnotations)─> sending
       ├─(WS message 按 client_message_id 确认)─> 清空该批，各 page 分组编号归 1
       └─(POST 失败 / 40s 未确认)─> 自查 → 真失败才 in_flight 并回 records 尾部, 重排编号, toast
```

- 进入 `sending` 的瞬间，`records` 里所有 `pending` 记录**移入 `in_flight`**；`sync` 后存活页签的图钉立即消失，待发卡片立即清空（用户看到「发出去了」）。
- `in_flight` 不参与 `sync`、不参与重放、不计入 `pending_count`、不可编辑、不在卡片上显示（否则用户会以为没发出去）。
- 成功：丢弃 `in_flight`；期间没有新记录则卡片与工具栏一起消失。
- 失败：`in_flight` 并回 `records` 尾部、按 page 分组重排编号、`sync` 让图钉回来、卡片回填，toast 提示可重试。
- 发送阶段**没有截图步骤**：图在 `save` 时已经截好挂在记录上（§4.8、D2），发送只做上传。上传失败不阻塞发送——该条标 `screenshot_unavailable`，消息照发。
- `orphan` 记录**有图**（保存时截的），发送时和活着页签的记录一视同仁，只是不再有新的图可更新。
- 记录副本（`annotations`）在移入 `in_flight` 时就已从原生快照生成，页签此后关闭不影响发送（Q33）。
- 没有「卡片在输入框等待」这一态——那是被否掉的 U1 方案 A。

**用户文本与批注合并成一条消息**

composer 里可能同时有用户敲的话和一批批注。合并成**一条**消息，避免两次 POST 触发两轮 turn：

| 用户文本 | 消息 `text` |
|---|---|
| 空 | `Page annotations\n{json}`（与基线一致） |
| 非空 | `{用户文本}\n\nPage annotations\n{json}` |

解析层相应放宽：不再要求 `startsWith(HEADER)`，改为「文本末尾存在 `\nPage annotations\n` 分隔段」。卡片渲染时只展示用户文本 + 批注卡片，JSON 段被遮蔽。

**快捷键语义**

| 位置 | 按键 | 行为 |
|---|---|---|
| 浮窗描述框 | `Enter` | **添加**：保存当前批注，生成图钉挂在页面，等待发送 |
| 浮窗描述框 | `Ctrl+Enter` | **保存当前 + 触发整批发送**（U4 = a），等同于「添加完再从任一触发点提交」 |
| 浮窗描述框 | `Shift+Enter` | 换行（批注描述是多行文本） |
| composer | 发送按钮 / 既有发送快捷键 | 发送（用户文本 + 整批批注，合并一条） |

- 空描述不允许「添加」：`save` 仍是必填（现状 `description` 非空、≤16384）。
- `Ctrl+Enter` 时若当前浮窗描述为空，则**只发送已有整批**，不报错（等价于用户直接点工具栏或输入框的发送）。

---

## 4. 关键机制

### 4.1 命令面：quiet vs busy

现状 `PageAnnotationSession::prepare()` 对**所有** action 统一置 `busy_ = true`（`page_annotation_session.hpp:300`），连 `read` 都占闸；而 `design.md:365-366` 明写这些是 quiet 命令。这是实现偏离设计，按住原图的手感直接受害（Windows 每命令 3 跳 CDP）。

拆成两档：

| 档 | action | 闸门 |
|---|---|---|
| quiet | `read` `preview_text` `preview_style` `original` `hide_markers` `set_modal_suspended` `sync` | 无 `busy_`；只做 `document_id` / `epoch` / 目标校验；允许并发、允许覆盖 |
| 事务 | `start` `exit` `cancel` `save` `add_target` `add_region` `select_target` `select_region` `discard` `reapply` `open_record_popup` `update_description` `delete_record` | `busy_` 串行 + `sequence` 防迟到 |

quiet 命令的返回值仍走快照路径，但**失败不得中断 WebUI 的 read 循环**（现状 `PageAnnotations.jsx:43-46` 一次 `ok:false` 就 `return`，定时器不再排，导致页面点击永久不回传）。

### 4.2 `sync`：原生全量下发图钉

页内 runtime 不再自己决定「画哪些图钉、编号是几」。每次记录集合变化（save / **update_description** / delete / reorder / discard / reapply 成功），原生向**该记录所属页签**下发：

```json
{"action":"sync","document_id":"...","pins":[
  {"record_id":"...","number":1,"state":"live","target_locator":"...","region":null,"bounds":{"x":0,"y":0,"width":0,"height":0}}
]}
```

页内按列表整体替换 `pins`（幂等）。删除一条 → 原生重排 `number` → 下一次 `sync` 全量刷新。这条同时解决了「删了记录图钉还在」的现存缺陷（当前 runtime 没有 `remove`）。

### 4.3 baseline 语义

必须改成「首次触碰值」，否则两个消费方看到的世界不一致：

- 现状样式：`select()` 里 `baseline = computed(target)`，而 adopted sheet 已含上轮 saved 声明（`page_annotation_style_runtime.hpp:102-110`）→ 白→红→蓝 会记成 `红 → 蓝`。
- 现状文本：`old_value = originals + saved`（`page_annotation_text_runtime.hpp:161-165`）→ 同理。

改法：

- 样式层 `baseline` 改为 **per-property、只在 saved/draft 之外取值**：`baselineOf(target, field)` 读取时先临时忽略本层构造样式表，或用「首次 select 时快照 + 之后不再覆盖」。
- 文本层 `old_value` 的基准改为 `originals`，不叠加 `saved`。
- **baseline 必须写进记录**，不只留在页内（重放与「按住原图」都要用）：`differences[]` 增加 `baseline_value`，或记录级 `targets[].baseline`。考虑到 `differences` 已经承载 `old_value`，且 Q20 后 `old_value` 就等于 baseline，**不再加字段**，但要在文档与测试中把语义钉死：`old_value` = 页面原始值，永不为中间态。

### 4.4 按住原图

页内已实现 `markersVisible = !markersHidden && !original`（`page_annotation_runtime.hpp:171`），原图态图钉与选区框自动隐藏、记录与编号保留。要补的只有前端时序：

1. `pointerdown` 立即 `original{enabled:true}`，不节流、不等 Promise。**不接键盘**：工具栏按钮吃掉 Enter 的默认激活（`preventDefault`），Space 不绑定；无障碍维度上它是 `aria-pressed` 之外的瞬时动作，靠按钮的 `title`/`aria-label` 说明「按住查看原图」。
2. 释放事件三路（`pointerup` / `pointercancel` / `pointerleave`）+ 两路兜底（`blur`、`visibilitychange`），任一触发即 `enabled:false`。
3. 按住期间：
   - 工具栏按钮置 `data-held`（不用 `aria-pressed`，那是切换语义）；
   - 浮窗与样式面板 `visibility:hidden` + `pointer-events:none` + `inert`（**不卸载**，不触发 `notifyNativeSurfaceOverlayChange()`）；
   - 原生侧拒绝 `preview_*` / `save`。
4. 样式层 `setOriginal(0→1)` 时把当前 draft 的差异快照进 `comparison`，释放后原样恢复（已实现）。

### 4.5 隐藏标记

改成整层隐藏，不再逐元素挑可见性：

```js
case 'hide_markers': overlayRoot.style.visibility = next.enabled ? 'hidden' : 'visible';
```

理由：现有实现逐个判断 `marker` / `pin.outline`，新加任何 overlay 元素（hover 卡就是这样漏掉的）都必须记得加条件。整层隐藏从结构上免疫。

锁存语义，图标 `aria-pressed` 反映状态；与按住原图正交，可组合。

### 4.6 重放（reapply）

触发：`NavigationCompleted`（Windows）/ `didFinish`（mac）后，若存在 `replayable` 记录且 `page_url` 精确匹配，启动一轮**带观察窗口的重放周期**。

算法（页内执行，原生给数据）：

```
attempt(records):                          // 每个窗口 tick 跑一次
for record in replayable:
    el = locate(record.locator)            // CSS 路径 → testid → aria-label → 文本指纹 → 同级索引
    if !el: mark pending, continue         // 还没渲染出来，不是 stale
    for each difference:
        if difference.property == "text":
            if liveText(el) != difference.old_value: stale; continue
            applyTextDiff(el, difference.old_value, difference.new_value)
        else:
            if computed(el)[property] != difference.old_value: stale; continue
            styles.apply(el, property, difference.new_value)
    record.state = live
sync()                                     // 全量刷新图钉，编号沿用 record.number
```

**观察窗口**（U2 裁决）：`didFinish` 后立即 `attempt()` 一次；此后用 `MutationObserver`（节流 300ms）驱动重试，窗口 3 秒，最多 10 次。
窗口内「元素还不存在」不算失败，只是继续等；窗口结束仍未定位到，才判 `stale`。

四条硬约束：

1. **值相等闸**：实时值必须等于 `old_value` 才写。否则会盖住 agent 已改好的代码结果，让页面说谎——这比不重放严重得多。
2. **编号沿用**，不重新分配。
3. **不做跨 URL 重放**。`page_url` 精确匹配（含 query），SPA 路由变化视为不同页面，记录留在 `replayable` 等用户回去。
4. **`in_flight` 不参与重放**（已点发送，正在等 WS 确认）。

为什么要有窗口而不是「一次定生死」：`didFinish` 不等于 DOM 就绪。React/Vue 的 SPA 在这个时刻常常只有骨架屏或空容器，数据还在接口里。一次定位失败就把记录判死，用户会看到「刷新后我的修改没回来」，而且看不出来为什么。

闪烁：接受（Q32）。就绪后一次 CDP 往返的延迟，与 Codex 同级。若要消除，改 document-start 注入 + 元素插入时微任务套用，但会引入「页面 CSS 未加载导致 `getComputedStyle` 读到默认值」的误判，本期不做。窗口机制让「元素晚到」也能补上，这是 (b) 相对一次性的主要收益。

**stale 的可见性**：页面上画不出图钉（没有坐标），但工具栏必须显示「N 条已失效」，记录在批注列表里置灰、可查看、可发送（带 `target_invalid`）。静默隐藏会让用户以为批注丢了，然后重改一遍 → 重复记录 → agent 改两遍。

### 4.7 元素定位 → 源文件

**零注入**，只在 `save` 时读一次。

1. 隔离世界给目标打随机属性 `data-ace-hit-<hex>`（样式层已有同款手法 `data-ace-preview-*`）。
2. 主 world 执行：

```js
const el = document.querySelector('[data-ace-hit-<hex>]');
const f = el[Object.keys(el).find(k => k.startsWith('__reactFiber$'))];
let n = f, stack = [];
while (n) { if (n.type?.name) stack.push(n.type.name); if (n._debugSource) break; n = n.return; }
return {source: n?._debugSource ?? null, components: stack.slice(0, 8)};
```

依据：`__reactFiber$` 键在 dev/prod 构建都存在（`react-dom.development.js:11481`，production 构建同样命中）；`_debugSource` 仅 dev（`react-dom.development.js:13288/13942/13963`）。

3. 路径归一化（原生或消息构造侧）：剥 `?t=` / `?vue&type=`；`/src/x.tsx` → 尝试 `<workspace>/src/x.tsx`；`/_next/static/chunks/**` 直接丢弃。
4. Vue 3 dev：`el.__vueParentComponent.type.__file`；Svelte dev：`el.__svelte_meta.loc`。
5. **拿不到 file:line 时给组件栈**（Cursor 同款），它不是空信息，agent 可以按名字 grep。
6. 硬约束：定位只作提示，路径必须落在 workspace 内且文件存在；改动前必须先说「我改的是 X」；定位不到必须停下来问。

### 4.8 截图（保存时裁剪，D2）

**时机前移**：不是发送时截，而是**每次 `save` 截一张**。保存那一刻页面正停在目标上，是坐标最准的时刻；等到发送时页面可能已经导航、重放、或干脆关了。

```
save（同步返回，不等捕获）
 → 记录立刻入库并广播（thumbnail 为空，screenshot_pending:true）
 → 后台：视口捕获一次（Windows Page.captureScreenshot 视口，scale = 1/dpr；mac takeSnapshot 视口）
 → 后台：按目标 rect（并集 + padding）裁剪（坐标即 CSS 像素，不乘 DPR）
 → 后台：缩到宽 ≤ 512px → record.thumbnail（PNG data URL）
 → 后台：同一张视口原图另存为该页的 page_shot（每页只留最新一张，供已发消息卡片）
 → 回写记录 + bump revision + 广播（卡片缩略图与已发卡片配图就位）
 → 失败 / 2s 超时：screenshot_unavailable:true，不重试，save 本身不受影响
```

- **异步（D10）**：`save` 是直接操作动作，用户对它的感知是「立刻」。捕获串在返回路径上会让图钉出现时间取决于 CDP 往返，而缩略图并不紧急。异步也让「捕获失败」退化为一次字段更新，而不是一次交互失败。
- **`scale = 1/dpr` 是硬要求（#208 实测）**：DPR2 下不做修正，捕获图是 5120×2880（4.6 MB PNG），保存路径 P95 达 238 ms；做了修正是 2560×1440、P95 37 ms。见 `2026-11-22-html-page-annotations-t0-findings.md`。

- 一次捕获出两张图，分工固定：
  - `record.thumbnail`（局部，≤512px）——待发卡片的 24px 缩略图 + 发送时该条的 attachment；
  - `page_shot`（整屏，按 `page_id` 存在会话级 store，后来覆盖前者）——**只**给已发消息卡片做配图，**不进消息载荷**。整屏图若逐条随消息发出去，既会挤占模型上下文，也会把同一页重复传 N 次。
- **发送时不再重复捕获**（D2）。
- 目标在视口外 / 捕获失败 / mac 拿不到快照 → `screenshot_unavailable: true`，卡片退化为 `tag` + 元素文本片段（D2 的降级形态），发送时该条无附件；此时 `page_shot` 仍可能是好的，消息卡片照常出图。
- `orphan` 记录**仍有图**——两张图都是保存时留下的，与页签在不在无关。这消掉了「页签关了就没图」的老问题。
- 删除批注不触发重截：`page_shot` 保留删除前的画面，直到该页下一次 save。这是可接受的已知偏差（PRD §11）。
- 不再需要 `captureBeyondViewport` + `cssContentSize` 的整页捕获：视口够用，目标在视口外本来也没有坐标能裁。
- 上传仍在发送阶段，base64 → attachment，失败降级不影响消息。

**代价**：每次 save 多一次视口捕获（Windows 一次 CDP 调用，`executionContextId` 已缓存）。用户保存批注是低频动作，可接受；换来的是「发送时零捕获」和「关页签仍有图」。

### 4.9 发送链路

**基线现状**：`PageAnnotationSendBar.jsx` 点「发送」直接调 `sender.send({snapshot, sessionId, api, page})`，消息立即进会话——**已经是直发，没有待确认卡片**。所以本节的工作量是「换入口 + 砍回执」，不是「改交互」。

砍掉：

- `annotation_batch_id` 与批次冻结、`reconcileOnly` 重试对账、服务端 `annotation_batch_not_accepted`（404 协议）
- 批次级整页截图 + `fullPage === true` 断言（mac 上必失败）
- 原生 `acknowledge` / `capture` / `validate` / `release`（租约机制）
- `PageAnnotationSendBar` 的「重试」三态（`idle/working/error` 保留，但失败即回滚记录，用户重按一次）
- **工具栏发送按钮里的发送链路**（不是按钮本身）：`capture`/回执/租约全部删，按钮改为调用 `submitWithAnnotations()`

新链路：

```
任一触发点（输入框发送 / 工具栏发送 / 浮窗 Ctrl+Enter 先 save 再触发）
 → submitWithAnnotations()：一次性取「composer 文本 + 会话内全部待发记录」
 → records 全部移入 in_flight → sync（存活的页签图钉立即消失，待发卡片立即清空）
 → 上传每条记录的 thumbnail（save 时已截，这里只上传，不捕获）→ attachment
 → 组装消息：text = 用户文本 + "\n\nPage annotations\n" + payload JSON，attachments = [截图 id]
 → POST /api/sessions/:id/messages（不带 annotation_batch_id）
 → 202 只表示入队，不算成功
 → WS message 事件按 client_message_id 确认
 → 丢弃 in_flight；若 records 为空，各 page 分组编号归 1、卡片与工具栏隐藏
   （40s 内未确认 → 见下「超时自查」）
```

**超时自查（U5）**：40s 没等到 WS `message` 时，**不要直接回滚**——先查一次这条消息到底有没有落库：

```
自查 GET /api/sessions/:id/messages?client_message_id=<id>
 ├─ 查到 → 视为成功：丢弃 in_flight，正常收尾（说明只是 WS 事件丢了）
 └─ 查不到 → 才回滚：in_flight 并回 records 尾部、重排编号、sync 恢复图钉、toast「发送失败，可重试」
```

理由：`client_message_id` 已经写进消息 `metadata` 落库（`routes_sessions.cpp:524`），WS 回推时也带回来（`:734-738`）。没有自查的话，一次 WS 抖动就会让用户重发，agent 收到两条一样的批注、把同一处代码改两遍。服务端需要补一个按 `client_message_id` 的查询（读 `metadata`，不需要新列）。

**消息载体沿用基线的「文本内嵌 JSON」**（`web/src/lib/pageAnnotationMessage.js:3` 的 `"Page annotations\n"` 头 + payload），不新增 `contexts` 类型。理由：

- 基线已跑通，前端已有 `parsePageAnnotationMessage` 解析并渲染成卡片、`contentPartsWithoutAnnotationScreenshot` 剥截图，改 `contexts` 等于把可工作链路推倒重写。
- 模型直接读结构化 JSON，字段边界比 prompt 文本块清晰。
- 零后端改动（除删 `annotation_batch_id` 校验、加超时自查查询）。

代价（写进风险）：消息 `text` 里存的是 JSON，导出/搜索/其他客户端会看到原文。主 UI 由解析层遮蔽，可接受。

- 发送目标 = 卡片所属会话，**不提供会话选择器**。
- 误发不可逆（Q35）。缓解只靠位置与视觉：它就是用户熟悉的那个发送按钮，不做长按/双击等隐藏手势；发送前卡片上能看清这次要发几条、发往哪些页面。
- 页签在发送途中关闭：payload 已冻结成副本，发送不受影响。

**消息历史里的卡片**（`PageAnnotationMessageCard.jsx`）——**只读**：

- 结构：**页面整屏截屏**（单击看大图）+ 下方一行「页面标题 + `N 条注释`」。
- `N 条注释` 是悬停入口：浮出该页明细，每条一行「元素标签（`div`/`input`/`p`）+ 描述」，最多 5 条，超出时列表内部滚动。
- 一条批注有多个目标时，元素标签取主目标；一条消息涉及多页时按页面堆叠「截屏 + 标题 + 标签」，每张图只展开本页条目。
- 不给铅笔、不给垃圾桶、不给「回到页面修改」。已发消息是 transcript，agent 已按当时描述执行；能改等于改历史，且系统没有任何编辑消息的 API。
- **刻意不再逐条平铺描述与缩略图**：待发卡片要回答「这条是不是我想发的」，所以逐条、带缩略图；已发卡片要回答「当时页面长什么样」，所以整页一图、明细收进悬停。两者不是同一套布局的两种状态。
- 想做新一轮修改 → 回页面重新批注（新记录），这是显式且留痕的路径。

### 4.10 草稿暂存

浮窗的 `description` 按 **`page_id + target_locator`** 存进 WebUI 内存 store（`Map`），组件卸载、文档替换（含 HMR 重载）都不丢。

- 不用 `document_id` 做 key：HMR 在开发时频繁触发，`document_id` 一变草稿就没了，而这恰恰是用户正在打字的时候。
- 兜底：重载后若 target 定位不到，草稿保留但不绑定元素，浮窗以「草稿（未绑定）」形态可继续编辑或丢弃。
- 仅内存，不落盘，不进原生。

### 4.11 待发批注卡片与逐条编辑（U3）

**卡片是什么**：会话输入框上方的一条卡片，列出**该会话所有页签**尚未发送的批注。它不是消息、不占 transcript，数据源是原生 `SessionAnnotationStore`，不是消息载荷。

**数据从哪来**：现状 `AGENT_BROWSER_STATE_EVENT` 只带当前面板页签的 `annotations`（`AgentBrowserPanel.jsx:83` 按 `page_id` 过滤），面板外的页签上报不到。要支持跨页签聚合：

- 原生在**任一页签**的记录集合变化时都发状态事件 `{session_id, page_id, annotations}`，不再只发面板页签；
- WebUI 新增 `pageAnnotationPendingStore`，按 `session_id → page_id → records` 归并，并单独维护 `orphans[]`；
- 卡片订阅该 store。页签关闭时原生推 `{page_id, annotations: []}`，同时把该页记录挪进 `orphans[]`——卡片上一行都不少。

**一行长什么样**（对应设计稿）：

```
[缩略图 24px] [tag: div]                        [铅笔] [垃圾桶]
描述文本
```

缩略图就是 `record.thumbnail`——保存批注时按 rect 裁剪的那张（D2），与消息里那张附件是同一张。窗口宽度不够时缩略图先让位（保持 tag + 文本），不撑破卡片。

**条数与滚动**（D3）：卡片最多展示 **5 行**，超出部分在卡片内部滚动（`max-height` + `overflow:auto`，不是分页、不是折叠成数字）。底部聚合 chip `N 条注释` 可把整卡折叠/展开。跨页签时按页签分组，组头是该页标题/URL（`orphan` 组标「页面已关闭」）。

**铅笔：两条路径**

路径 A —— 页签还活着：

```
点铅笔
 → selectAgentBrowserPage(page_id)              // 激活页签
 → 若该页不在批注模式，隐式 start()             // D8：要拉回，页面进入批注模式
 → open_record_popup{record_id}                 // 新 action
 → 页内：目标滚动进视口 → 浮窗以「编辑已有记录」态打开
     · 描述框 value = record.description，光标置于末尾并 focus()
     · 差异从 record.differences 还原成 draft，样式面板可用（「调整」显示）
     · 图钉高亮，不新增编号
 → 保存 → update_description{record_id, description}（改过样式则整条替换）→ sync + 卡片刷新，编号不变
 → 取消 → 丢弃 draft，记录恢复原状
```

路径 B —— 页签已关闭（`orphan`）：

```
点铅笔
 → 无页签可激活 → 弹居中 modal（PageAnnotationEditDialog，视觉沿用浮窗）
 → 只有：描述框 + 删除 + 取消 / 保存
 → 「调整」隐藏（adjustable=false）
 → 保存 → update_description{record_id, description} → 卡片就地更新
 → target 冻结：locator / region / differences 全部只读展示，不允许改
```

**`adjustable` 的判定只有一条**（两个入口共用，禁止各写一份）：

```
adjustable = record.state === 'live' && 页签存活 && document_id 匹配
```

`replayable` 也一并隐藏——重放还没成功，样式面板打开会读到错的 baseline，改出来的 `old_value` 就是脏的。

**D8 的副作用要认**：隐式 `start()` 会把页面拉进批注模式，用户之后在页面上点任何地方都会新建一条批注。这是有意为之——从卡片点铅笔的人本来就是来改批注的；且 `start` 会复位 `original` / `markers_hidden`，视图从"上次的临时状态"回到"批注中"的正常状态，比只开浮窗更符合预期。

**路径 B 为什么只能改文本**：没有 document、没有 baseline、没有能挂 `adoptedStyleSheets` 的地方。硬留「调整」按钮只能点了报错，所以直接不给。

**编辑途中页签被关掉（D7）**：不弹窗、不打断输入。浮窗里已敲的内容写进 `pageAnnotationDraftStore`（key `page_id + target_locator`），记录转 `orphan`，卡片行标「页面已关闭」；用户下次点这行的铅笔，直接进路径 B 的弹窗，描述原样还在。

**删除**

- 行上的垃圾桶 = 删除这条**待发**记录（`delete_record`），`sync` 重排编号。
- 立即生效 + 5s 撤销 toast（撤销按原位置插回、重排编号）。不做二次确认弹窗：批注是轻量草稿，弹窗打断比误删更烦。
- `orphan` 记录的删除不需要页签，直接改会话存储。

**保存后的一致性**：一次编辑同时落到三处，靠一个事件扇出，禁止各自维护副本——

1. 原生 `SessionAnnotationStore`（权威）
2. 存活页签的图钉与预览（`sync`）
3. 输入框上方的卡片（store 订阅）

**已发消息卡片只读**：不给铅笔、不给垃圾桶、不给「复制」、不给「回到页面修改」（D6：一个按钮都不给）。卡片本身只有「整屏截屏 + 页面标题 + `N 条注释`」，明细在悬停浮层里，浮层内的条目同样不可编辑。消息是 transcript，agent 已按旧描述执行过，能改就等于改历史；留一个「复制」只会让人以为下一步能改。用户想「改完再让 agent 做一次」，就是在页面上重新批注一条新的——这件事必须是显式的、留痕的。

### 4.12 记录标识与定位符（D9）

**结论**：`record.id` 不能和文档绑定。现行 `<document_id>:annotation:<n>` 在刷新后结构性失效——`document_id` 内含自增 epoch（`page_annotation_session.hpp:213`），替换时 epoch+1 且 `document_id_` 清空（`:143-168`），记录 id（`:381`）指向一个不存在的文档代；页内 runtime 按 `document_id` 建键，换文档即 `dispose()`（`page_annotation_runtime.hpp:9-12`），图钉随之消失（`:359-375`），元素标识 `target-N` 是新 runtime 里重新数出来的序号（`:28, 217`）。完整因果链与实测见 `2026-11-22-html-page-annotations-t0-findings.md`。

```
record.id       = <page_id>:annotation:<n>     // 会话内稳定，n 按 page_id 分组连续
record.page_id  = 所属页签
record.page_url = 保存时的 URL（重放要求精确匹配）
record.document_id = 保存时的文档代（只用于判定 live / replayable，不做主键）
record.locator  = 跨文档可用的结构定位符
```

**`locator` 是新增字段**，`save` 时生成一次，之后只读。此前 `targets[].locator` 只有 `tag#id`（`page_annotation_runtime.hpp:97`），对没有 `id` 的元素退化成 `div`，撑不起 §4.6 的 `locate()`。生成顺序（逐级降级，取第一个稳定命中的）：

1. `data-testid` / `data-test` / `aria-label` 唯一命中；
2. CSS 结构路径：从目标到 `documentElement` 的 `tag:nth-of-type(k)` 链（跳过 `data-ace-*` 注入节点与批注 overlay）；
3. 文本指纹：`tag` + 归一化文本前 80 字符的滚动哈希；
4. 同级索引：父路径 + `nth-of-type`，最弱，只在 1~3 都失败时用。

**`target.id`（`target-N`）只在同一 runtime 内有效**，不跨文档、不进主键；跨文档一律走 `locator`。

**`stale` 的判定不依赖 locator 命中率**：定位不到、定位到但实时值 ≠ `old_value`、文本节点结构变化，三者都判 `stale`（§4.6 的四条硬约束不变）。

---

## 5. 数据契约

### 5.1 记录

```json
{
  "id": "<document_id>:annotation:<n>",
  "number": 3,
  "page_id": "page-...",
  "page_url": "http://127.0.0.1:5173/pricing?tab=2",
  "page_title": "Pricing",
  "description": "这个标题太小了",
  "targets": [{
    "locator": "body>div#root>main>section:nth-child(2)>h2",
    "tag": "h2",
    "text": "Order summary",
    "source": {"file": "/src/components/Card.tsx", "line": 42, "column": 7},
    "components": ["CardTitle", "Card", "App"]
  }],
  "differences": [
    {"target_id": "...", "property": "font-size", "old_value": "14px", "new_value": "18px"},
    {"target_id": "...", "property": "text", "old_value": "Order summary", "new_value": "订单摘要"}
  ],
  "region": null,
  "state": "live",
  "thumbnail": "data:image/png;base64,...",
  "screenshot_unavailable": false,
  "status": "pending"
}
```

`state`: `live | replayable | stale | orphan`。`number` 是**页内序号**（同一 `page_id` 分组内 1..n），跨页签混批时会重复，靠 `page_url` 消歧。`status` 不落记录：发送中由 `in_flight` 数组单独表示，已发送即从 `records` 删除。

`thumbnail`：**保存批注时**按目标 rect 裁剪的小图（宽 ≤512px，D2；卡片按 24px 渲染，消息里保持可读），待发卡片缩略图与消息附件共用它；`screenshot_unavailable: true` 表示保存时就没截到（目标在视口外 / 捕获失败）。`orphan` 记录不影响这两个字段——图在保存时就定格了。记录本身序列化进消息时**去掉 `thumbnail` 的 base64**，只留 `screenshot_ref` 指向附件。

### 5.2 消息载荷（嵌在消息文本里）

沿用基线的承载方式：消息 `text` = `"Page annotations\n"` + JSON（`web/src/lib/pageAnnotationMessage.js:3`），截图作为普通 attachment 随消息（`contentParts` 的 image part）。用户同时敲了话时，`text` = `{用户文本}\n\nPage annotations\n{json}`；解析层按**末尾分隔段**识别，不再要求 `startsWith`。

```json
{
  "page": {"url": "...", "title": "..."},
  "annotations": [ /* 记录全文，含 state / target_invalid 标记 */ ]
}
```

- 不加 `type`/`id`：消息本身就是批注批次，解析层按文本头识别。
- `annotations` 是冻结副本：页签关闭后仍完整可读。
- 跨页签混批时 `page` 取第一条记录所在页（向后兼容），**每条记录自带 `page_url` / `page_title`（Q23）才是权威**；卡片与模型都按 `page_url` 分组。
- 截图**按条一张**（Q25/Q28 + D2）：直接取 `record.thumbnail`（保存时按 rect 裁剪），文件名 `page-annotations-<record_id>.png`，与记录用 `screenshot_ref` 关联；无图的记录标 `screenshot_unavailable`（`orphan` 记录**不在此列**，它保存时已经截过）。
- 一条消息只发一次 POST：用户文本与批注不会拆成两条（拆开会让 agent 多跑一轮 turn）。

### 5.3 模型侧所见

模型读到的就是消息 `text` 原文，即 `Page annotations` + JSON。字段含义：

```
page.url / page.title           当前页面
annotations[].number            本轮展示序号
annotations[].description       用户写的批注
annotations[].targets[].locator 唯一 CSS 路径（重放/定位用）
annotations[].targets[].source  {file,line,column}（React dev 才有，可能为 null）
annotations[].targets[].components 组件栈（prod 也有）
annotations[].differences[]     {property, old_value, new_value}
annotations[].state             live | replayable | stale
annotations[].target_invalid    true = 记录来自别的文档，不要按当前 DOM 找
```

---

## 6. 改动清单

### 原生

| 文件 | 改动 |
|---|---|
| 新增 `session_annotation_store.hpp` | 会话级待发集合：`records[]`（每条带 `page_id`/`page_url`/`page_title`/`state`/`thumbnail`）、`in_flight[]`、按 `page_id` 分组的 `next_number`；提供 `delete_record` / `update_description` / `adopt_orphans` |
| `page_annotation_session.hpp` | 记录四态替代 `valid`；记录读写转发到 `SessionAnnotationStore`，自身只留选区/预览/`document_id`；quiet/事务两档闸门；删除单条 + 重排；`sync` payload；`reapply` 编排；新增 `open_record_popup` / `update_description` / `delete_record`；`replace_document` 转 `replayable` |
| `page_annotation_runtime.hpp` | 增加 `sync` / `remove` / `reapply` 分支；`hide_markers` 改整层；保存时输出扩展 locator；打 `data-ace-hit-*`；移除内部编号逻辑 |
| `page_annotation_style_runtime.hpp` | baseline 改首次值；`apply` 支持重放层（与 saved/draft 分离） |
| `page_annotation_text_runtime.hpp` | `old_value` 基准改 `originals`；重放用 diff 应用 |
| `page_annotation_host_adapter.hpp` | 删除 `capture` / `validate` / `acknowledge` / `release` 分支；`screenshot` 从**发送链路移到 `save`**（视口捕获 + 按 rect 裁剪 + 缩到 128px） |
| `agent_browser_native_commands_win.cpp` | 缓存 `executionContextId`（只在 `replace_document` 后重建），省掉每命令 3 跳里的前两跳 |
| `agent_browser_capture_cdp.cpp` | 去掉 `captureBeyondViewport` + `cssContentSize` 整页路径；改为**视口捕获 + 按 rect 本地裁剪**（save 时调用一次） |
| `agent_browser_host_mac.mm` / `_win` | 去掉批次相关；**`close_page` 不再析构记录**，把该页记录转入 `SessionAnnotationStore` 的 `orphans` 并广播；状态事件改为广播会话内**所有**页签的 `annotations`（不再只发面板页签）；`close_page` 时若有在途发送直接失败 |

### 前端

| 文件 | 改动 |
|---|---|
| `PageAnnotationSendBar.jsx` | **保留发送按钮，换掉实现**：不再走 `pageAnnotationBatchSender`，改为调 composer 的 `submitWithAnnotations()`（Q35）；按钮显示会话级总条数，禁用条件与输入框一致（`!sessionId` / 空批注且无文本 / 发送中）；按住原图补 `blur`/`visibilitychange`、删掉键盘 Space/Enter 路径（Q2 修订：纯指针）；`aria-pressed` 只留给隐藏标记 |
| 新增 `PageAnnotationPendingCard.jsx` | 输入框上方的待发卡片：按页签分组、每行 缩略图 + tag + 铅笔 + 垃圾桶 + 描述、底部 `N 条注释` chip |
| 新增 `PageAnnotationEditDialog.jsx` | `orphan` 记录的编辑弹窗：只有描述框 + 删除 + 取消/保存，`adjustable=false` 时隐藏「调整」 |
| 新增 `pageAnnotationPendingStore.js` | `session_id → page_id → records` + `orphans[]`，订阅原生状态事件，供卡片与弹窗读取 |
| `PageAnnotations.jsx` | `read` 失败不终止轮询；按住期间浮窗 `inert`；草稿按 page+locator 暂存；挂载无条件 `read` 一次；`Enter`=添加 / `Shift+Enter` 换行；浮窗支持「编辑已有记录」态（描述回填 + `focus()` + 图钉高亮） |
| `pageAnnotations.js` | 去掉批次相关；新增 `reapply`；controller 支持 quiet 命令并发 |
| `pageAnnotationBatches.js` | **删除**，替换为 `pageAnnotationMessage.js`（载荷构造 + 解析 + 发送确认；截图在 save 时已由原生生成，此处不做裁剪） |
| `pageAnnotationMessage.js` | 解析放宽到「末尾 `\nPage annotations\n` 分隔段」；卡片按 `page_url` 分组；保留 `comments[]` 形状以兼容现有测试 |
| `PageAnnotationMessageCard.jsx` | **只读**：载荷解析 + 渲染「整屏截屏 + 页面标题 + `N 条注释`」；悬停标签浮出明细（元素标签 + 描述，最多 5 条，超出滚动）；不加任何按钮 |
| 新增 `pageAnnotationDraftStore.js` | 内存 Map（`page_id + target_locator` → 描述草稿），跨 `document_id` 存活 |
| `lib/selectionChatContext.js` | 不改（`page_annotation` 不走 `contexts`） |
| `ChatView.jsx` | 消息卡片渲染分发（只读快照，不读写 composer 状态）；**发送时把待发批注并入本次 POST**（用户文本 + payload 合并一条） |

### 服务端

| 文件 | 改动 |
|---|---|
| `routes_sessions.cpp` | 删除 `annotation_batch_id` 字段校验与 `annotation_batch_not_accepted` 404 分支；新增 `GET /api/sessions/:id/messages?client_message_id=` 查询（读 `metadata`，供 40s 超时自查） |
| `openai_provider.cpp` / `anthropic_provider.cpp` | 不改（JSON 在消息 text 里，模型直接读） |

---

## 7. 删除清单

- `annotation_batch_id` 全链路（前端、请求体、`metadata` 落库）
- `reconcileOnly`、`annotation_batch_not_accepted`（404 协议）
- 原生 `capture` / `validate` / `acknowledge` / `release` 与 `AnnotationNavigationGuard` 租约
- `pageAnnotationBatches.js` 及其测试
- `PageAnnotationSendBar` 里的「重试」按钮语义（失败就是失败，用户重按一次）
- 批次级整页截图 + `fullPage` 断言
- 页内工具栏**旧发送链路上的** `pageAnnotationBatchSender` 调用与「发送中/重试」三态文案（按钮保留，改为触发 composer 提交）
- 消息卡片的**全部按钮**（「回到页面修改」「复制」「重发」）——已发消息只读
- `PageAnnotationSession` 里的 `records` / `next_number` 成员（上提到 `SessionAnnotationStore`）
- `close_page` 里的 `pages.erase(page_id)` 记录析构路径

---

## 8. 测试矩阵

| 层 | 用例 |
|---|---|
| 原生模型 | 四态转换；删除后重排；`discard` 归 1；`replace_document` 转 `replayable` 且编号保留；quiet 命令不占 `busy_`；并发 `read` + `preview_style` 不互相拒绝；`in_flight` 不计入 `pending_count` 且不参与 `sync`/重放；**关页签 → 记录转 `orphan` 而非析构**；`orphan` 不参与 `sync`；按 `page_id` 分组编号 |
| 页内 runtime（node:test + vm） | `sync` 幂等；整层 `hide_markers`；原图态图钉隐藏、释放恢复；`reapply` 的值相等闸（相等写入 / 不等标 stale）；**观察窗口内元素晚到仍能补上**；文本 diff 重放；`Enter`=save / `Shift+Enter` 换行；`open_record_popup` 回填描述 + `focus()` + 滚动进视口 |
| 前端 | 按住三路径（up/cancel/leave）+ 两路兜底（blur/visibilitychange）+ **键盘不触发**（空格与 Enter 均不进 `held`）；read 失败后循环仍继续；草稿按 `page+locator` 跨 `document_id` 暂存；待发卡片跨页签聚合；`adjustable` 判定（live 才 true，replayable/stale/orphan 全 false）；铅笔路径 A（激活页签 + 打开浮窗）/ 路径 B（弹窗，无「调整」）；删除 + 5s 撤销；发送→`in_flight`→确认清空 / 40s 自查（查到即成功、查不到才回滚）；**三触发点等价**（输入框 / 工具栏 / `Ctrl+Enter` 各触发一次，断言 `submitWithAnnotations` 调用参数完全一致，且只产生一次 POST） |
| 集成 | save → 导航 → reapply → 编号不变；**关页签 → 卡片仍在 → 弹窗改文本 → 仍可发送（有图，图是 save 时那张）**；跨页签混批发一条消息；多会话各自页签互不影响；发送中关页签不阻塞；用户文本 + 批注合并成一条消息且解析正确；**发送阶段零捕获**（断点确认不再调 `captureScreenshot`） |
| 契约 | `Page annotations` 文本头 + payload 的前后端解析一致（构造端与 `parsePageAnnotationMessage`，含「用户文本在前」的形态）；`composer_content` 封闭枚举未被触碰；已发消息无任何可写入口 |
| 冒烟 | Windows 真实 WebView2：按住原图无闪烁、无卡死；mac：视口外目标发送不报错、明示未附图 |

---

## 9. 风险与未决

### 9.1 风险

| 风险 | 说明 | 缓解 |
|---|---|---|
| 记录上提改动面大 | `records` 从 `PageAnnotationSession` 挪到会话级，牵动发送、重放、`sync`、编号、序列化 | 先做纯搬运（行为不变）一次性落地，再加 `orphan`；两批分开评审 |
| 跨页签编号重复 | 每页从 1 开始，混批后消息里出现多个 `#1` | 每条自带 `page_url`（Q23）；卡片与模型都按页分组；`number` 只在页内唯一 |
| 重放盖住真实改动 | 值相等闸已覆盖「值变了」的情况，但整块结构重排仍可能误配 | 重放失败一律 stale，不猜 |
| 源文件定位失真 | dev 路径映射到 workspace 可能错文件；页面可伪造返回 | 只作提示 + 必须确认 + 定位不到就停 |
| mac 截图覆盖不足 | 保存时目标在视口外就拿不到快照（Q31 从"发送时降级"前移到"保存时降级"） | UI 明示；卡片退化为 tag + 文本片段；发送时该条无附件 |
| `sync` 频率 | 每次增删一次全量下发，Windows 3 跳 CDP；现在还要广播给所有页签 | 缓存 `executionContextId`；quiet 档不排队；只给**有记录变化**的页签发事件 |
| 会话归档后的孤儿页 | 页签保留但会话不可见，记录长期占内存 | 本期接受（用户明确要求页签不随归档关闭） |
| 误发不可逆 | 直发无二次确认（Q35），误点即让 agent 开始改代码 | 三个触发点都是用户熟悉的发送动作；按钮上显示总条数；不做隐藏手势 |
| 发送重试可能重复 | 40s 超时后若实际已落库，会重发 | U5 自查兜住主要路径；`client_message_id` 查询本身失败时按「未知」处理——不回滚，只提示 |
| `orphan` 记录越积越多 | 页签关掉后记录留在内存，用户也没地方看见（卡片折叠时） | 卡片默认展开；会话关闭即清空；`orphan` 记录带「页面已关闭」标记 |

### 9.2 未决问题：0 条

原先的 8 条细节问题（D1–D8）已全部裁决，按「问题 / 裁决 / 影响」记在 **§1.1**，不再单列。评审时若要翻旧账，看那里。

已定且刻意记录在案的（避免复评审时又被当成"还没想清楚"）：

| 曾经的问题 | 现在的位置 |
|---|---|
| 记录归属页签还是会话 | Q33 修订：归属会话，页签是渲染/编辑端点 |
| 发送入口在哪 | Q35 修订（二次）：组装唯一在 composer，触发三处（输入框 / 工具栏 / 浮窗 Ctrl+Enter） |
| 已发消息能不能改 | U3：不能，卡片只读、一个按钮都没有 |
| 40s 无确认怎么办 | U5：先按 `client_message_id` 自查再回滚 |
| 浮窗 `Ctrl+Enter` 语义 | U4：保存当前 + 发送整批 |
| 用户文本与批注发几条 | §3.4：合并成一条 |
| 截图何时截 | Q25/Q28 + D2：保存时截，发送时只上传 |

---

## 附录 A：竞品对照

| | 元素 → 源码 | 预览跨刷新 | 截图 |
|---|---|---|---|
| t3code | preload 主 world + `_debugSource` / `_debugStack`，输出 dev 路径但不归一化 | **不保留**（`teardown` 主动还原） | 视口捕获 + 本地裁剪，视口外返回 null |
| Agentation | `__reactFiber$` → `_debugSource`，dev-only | 不适用（无预览改值） | 无 |
| Cursor Design Mode | 组件名 + 组件栈 + xpath + fiber props（官方文档），不承诺 file:line | 文档：Apply 才写源码 | 保存时裁剪到元素 |
| Codex | 未公开 | **重放**（观察：刷新后旧值→新值闪烁） | 未公开 |
