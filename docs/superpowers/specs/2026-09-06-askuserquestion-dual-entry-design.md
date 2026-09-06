# AskUserQuestion 双入口设计（我要补充 / 以上都不是）

日期：2026-09-06
状态：**已评审通过并提交**（下一步：调用 `writing-plans` 生成实施计划，按第 8 节两批实施）
仓库：`/Users/liuxin557/DEV/acecode`

> 接续说明：本仓库 brainstorming 走 Architectural 路径，设计批准前不得写实现代码。本文即批准后的设计基线，实施时按第 8 节批次推进，落在同一 OpenSpec change `openspec/changes/ask-user-question-dual-entry/`。（设计初稿基于旧 calvinhxx v0.4.3 代码，已于 2026-09-06 对照 tmoonlight/acecode v0.9.x 主线审计修订，第 4/6/9 节含变更标注。**同日经 grill with doc 压力测试**：Q1 空文本拦截改「离开当前题就地拦截」（规则 3 作废）、Q2 payload 按 active 过滤、Q3 产物标记中文 + TUI 静态文案英文、W1-W3 文档偏差修正，决策表补 13-16。）

---

## 1. 背景与目标

**痛点**：当前 UI 自动追加的单一自定义项只有“追加”语义——手填文本必须与某个预设选项共存提交（daemon 侧解析旧版用 `parse_async_response`、v0.9.x 内联于 `routes_ws.cpp`，均以 `", "` 把 `selected` 与 `custom_text` 拼合），导致最终答案里混进用户并不认可的预设选项。用户原话：“other 更像是‘我要补充’”。

更糟的是两条路径行为不一致：TUI 同步路径提交自定义文本时直接覆盖答案、静默丢弃已勾选项；daemon 路径则合并。

**目标**：

1. 把自定义项拆成两个语义明确的入口，用户显式选择语义
2. 桌面 React 与终端 TUI 行为统一
3. **AI 完全知情**（方案 A）：模型能区分“用户否定了全部预设”与“用户在预设之外补充了信息”，而非只看到一段拼好的文本

**非目标**（明确排除）：不改动问题定义侧的校验（`validate_ask_user_question_args`，1-4 题 / 每题 2-4 选项 / header ≤12 码点 等规则保持不变）；不引入预设勾选快照与自动恢复；不为 TUI 新增回翻导航。

---

## 2. 术语表

| 术语 | 定义 | 代码/协议对应 |
|---|---|---|
| **预设选项** | 模型通过 `options[2..4]` 给出的候选项（控件，非答案） | `q.options[i].label`；协议 `selected` |
| **我要补充** | 追加式入口：不与预设互斥，可与已选预设共存提交 | React `supplement`；协议 `supplement_text` |
| **以上都不是** | 独占式入口：与预设互斥，激活即作废全部预设 | React `exclusive`；协议 `exclusive_text` |
| **激活** | 选中某个自定义入口 | `supplement.active` / `exclusive.active` |
| **手填文本** | 用户在入口输入框里敲的**内容** | `supplement.text` / `exclusive.text` |
| **提交答案** | 该题最终回传给模型的一串**产物** | 拼装纯函数的返回值 |

核心区分：「我要补充」「以上都不是」是**入口**（控件），手填文本是**内容**，提交答案是**产物**。三者不可混称。禁用含混旧称“自定义答案”“Other”。

---

## 3. 状态模型与交互规则

每题的应答状态由四部分构成，独立存储：

| 状态 | 内容 |
|---|---|
| `presets` | 已选预设集合（多选 = 集合；单选 = 至多一个） |
| `supplement` | `{ active, text }`；**active 为派生值**（GUI/TUI 均无勾选框：active = 文本非空 且 exclusive 未激活） |
| `exclusive` | `{ active, text }`；active 由**勾选**显式控制（GUI 勾选框 / TUI Space 切换） |

两个入口的 active 来源不同是刻意的：补充入口「打字即激活」，独占入口「勾选即激活」。

**交互规则**（GUI 鼠标 / TUI 键盘同构）：

| 操作 | 效果 |
|---|---|
| 激活「我要补充」 | 预设勾选**不动**；若「以上都不是」激活中 → 停用之，其文本保留置灰 |
| 激活「以上都不是」 | **立刻清空**全部预设勾选（所见即所得）；若「我要补充」激活中 → 停用之，其文本保留置灰 |
| 停用任一入口 | active 置否，文本**保留置灰**，不提交 |
| 独占态下点任一预设 | 退出独占（文本保留），该预设被选中；此后正常多/单选 |
| 编辑输入框 | 只改对应入口的 text，不触碰其他状态 |

**数据生命周期**：两条手填文本从敲下第一个字起就存在，只随“用户编辑”和“整组提交后的重置”变化——切换入口、切题往返、反复反悔都不清除。预设勾选**不复活**（进独占即清空，退出后重新勾）。React 支持回翻题目，回翻时状态原样保留；TUI 维持现状的顺序作答，本设计不因此新增回翻。

**提交校验**（grill 修订：**离开当前题时就地拦截**，原规则 3「允许跳过导航、最后统一拦」作废——TUI 每题 Enter 即提交并推进、不存在"整组提交"时点，"最后统一拦"在终端没有落点，会放任空文本独占直接回传模型）：

1. **离开当前题时**：任一 active 入口的 text 为空 → 阻止离开（TUI 回车 / GUI 下一步与提交），就地提示并聚焦到空输入框；填字或停用该入口后才能走
2. inactive 入口的文本不进入提交答案（见第 4 节 payload 规则）
3. 已作答判定 = ≥1 预设被选 或 任一入口 active——供导航/页脚勾选态渲染使用；exclusive active 但文本为空的题判为"已作答"（勾了就算答），但离开时被规则 1 拦下，不存在"带着空文本离开"的路径
4. GUI 最终提交按钮保留一次全量校验作状态机兜底（逐题已拦，理论不触发，故无需跨题跳转 UI）；TUI 无整组提交时点，每题 commit 即校验

**单选题（multiSelect=false）**：预设单选（点新换旧）；「我要补充」可与单选共存，也可独立提交；「以上都不是」照常清空。

---

## 4. 协议层

**请求结构**（前端 → daemon，每题的 answer）：

```json
{ "question": "...", "selected": [...], "supplement_text": "...", "exclusive_text": "..." }
```

- **非空即选中**：协议不传 active 标志；提交答案只含 **active 入口**的文本（grill Q2：React payload 按 active 语义过滤——exclusive active 时**丢弃** supplement_text，即使本地留有非空旧文本；被互斥压制的 inactive 文本仅供反悔，不进 payload，见第 5 节规则 4）
- **互斥兜底**：React 保证发往协议的最多一个文本字段非空；daemon 解析时仍防御性检查"两文本同时非空"（防外部直连 API 的调用方），命中则以 `exclusive_text` 为准并记日志
- `custom_text` 旧字段移除（前后端同版本发布，无过渡期兼容负担）

**最终提交文本**（模型看到的产物）：

| 用户选择 | 提交答案 |
|---|---|
| 仅预设 | `预设A, 预设B`（现状不变） |
| 预设 + 补充 | `预设A, 预设B; 补充: 手填文本` |
| 仅补充（独立） | `补充: 手填文本` |
| 以上都不是 | `以上都不是: 手填文本` |

标记词用中文、与入口文案一致；分隔符用 ASCII（`; `、`: `），规避 TUI 宽度不稳定字形（AGENTS.md 要求）。标记词语言是单点常量，改英文只需一处。

**拼装逻辑收敛到 `format_ask_answers`**（v0.9.x 审计结论）：初稿假设"两条路径各拼各的：`format_ask_answers`(TUI) + `parse_async_response`(daemon)"——**与现状不符**。审计发现：

- `format_ask_answers`（`src/tool/ask_user_question_tool.cpp:180`，声明在 `ask_user_question_tool.hpp:42`）**已是 TUI 同步路径与 daemon 跨进程路径共用的统一拼装入口**：两条路径最终都经它生成工具 `output` 文本。它本身就是初稿想造的那个"纯函数"。
- daemon 侧解析**没有独立 `parse_async_response` 函数**，而是内联在 `src/web/routes/routes_ws.cpp:377`（`AskUserQuestionAnswer` 解析：读 `selected` + `custom_text`）。
- 真正的缺陷在 TUI：**`tui_ask_channel.cpp:57` 把 `custom_text` 硬编码空**，双入口文本实际由 `main.cpp:6722 commit_current_answer` 直接拼进 `ask_result_answers` 的 join 串（question→`"A; 其他: xxx"`），绕过了结构化字段——这正是"自定义文本与预设混在一起"痛点的 TUI 侧根因。

本设计做法（grill W3 修正）：`format_ask_answers` **只做 `"问题"="答案"` 的引号包裹、不加工答案内容**——双字段标记的真正拼装点在各路径**构造每题答案串**时。新增纯函数 `format_single_answer(selected_labels, supplement_text, exclusive_text)` → 带标记的答案串（`预设A, 预设B; 补充: x` / `以上都不是: x`，标记词中文为单点常量，见第 6 节注），TUI commit 与 daemon 解析后各自调用它以构造 map 的 value，`format_ask_answers` 保持原样只包外层。两条路径的答案串构造收敛到同一函数，不可能再分叉。

**TUI 为何不走协议**：终端模式下 ask 工具、overlay、答案存储在同一进程，工具弹出问题后 `ask_cv.wait` 阻塞，用户操作直接写入 `TuiState` 的内存字段，全程无序列化边界，因此不存在 JSON 协议。TUI 通过同一个 `format_ask_answers` 生成最终文本（改造后：TUI 也先把双入口文本填入 `AskUserQuestionAnswer` 再交给该函数），语义与 daemon 路径一致。

**改动点清单**：

1. 工具 schema 的 `options` description 改写（**仅一处**：`ask_user_question_tool.cpp:371` 的 `question_schema`，v0.9.x 已把 TUI 同步版与 daemon 版合并为同一定义，TUI/daemon 共用）：告知模型 UI 会自动追加「我要补充」和「以上都不是」两个入口，无需也不应写进 options
2. `src/session/ask_user_question_prompter.hpp` 协议注释（**35-41 行**）：`AskUserQuestionAnswer.custom_text` 拆为 `supplement_text` + `exclusive_text`、互斥规则、“非空即选中”
3. 新增拼装纯函数 `format_single_answer` + 两路径调用点替换：`routes_ws.cpp:377` 按双字段解析后调用它构造答案串、`main.cpp` commit 处按双字段拼装后调用它（取代硬编码空 `custom_text` 与 `ask_result_answers` join 串旁路）；`format_ask_answers`（`ask_user_question_tool.cpp:180`）保持原样
4. `docs/daemon-api.md:3154` 同步（协议 `answers[]` 的 `custom_text` → `supplement_text` / `exclusive_text`）

---

## 5. React GUI 设计（方案 C · 主输入区）

**布局**：选项列表（预设 + 「以上都不是」行）→ 分隔线 → 题目下方的「补充说明」常驻输入区。

- 「以上都不是」= 列表行：勾选框 + 标签 + 行内输入框，暖色（红）强调独占性
- 「补充说明」= 独立文字域：标签“补充说明（可选，随答案一起提交）”，**无勾选框，非空即激活**
- 置灰态：未激活入口的输入框以 dim 显示保留文本，并标注“已停用 · 内容保留，不提交”

**C 特有交互规则**：

1. 补充说明框打字即生效：非空 = 「我要补充」激活；清空 = 未激活
2. 勾选「以上都不是」→ 补充框置灰停用（内容保留）；点回置灰框继续输入 → 自动恢复，同时取消「以上都不是」的勾选
3. 置灰 = 不提交，不等于删除
4. **校验时机 = 离开当前题时**（点下一步 / 最终提交）：active 入口文本为空即就地红框提示并阻止离开；被拦的就是当前题，无需跨题定位跳转。exclusive active 时 payload 只发 `exclusive_text`，置灰补充框的旧文本不进 payload（grill Q2）

**已走查的六个状态**（用户确认通过）：默认 / 预设+补充 / 独占激活 / 反悔改选预设 / 校验报错（红框 + 阻止离开）/ 单选题形态。

---

## 6. TUI 设计（方案 2 · 分区式）

沿用 `src/tui/ask_question_overlay.cpp` 现有视觉语言（`▸` 焦点、`[x]` 多选、`(●)` 单选、底部 hint 行、`Custom answer` 输入态）。

**TUI 静态文案语言**（grill Q3 定稿）：overlay 现有静态文案全英文（`Other...`、`Custom answer (...)`），新增两个入口的标签、输入提示、错误提示**沿用英文**（`None of the above` / `Supplement note`），与 overlay 一致；产物标记词固定中文（`补充:` / `以上都不是:`，单点常量，见第 4 节）——接受"终端英文入口标签 vs 产物中文标记"的字面不一致，语义靠结构表达。桌面 GUI 静态文案维持中文。

```
 Question 1/3  [deploy]
 选择部署方式？（可多选）

   [x] Docker 容器部署
   [ ] 二进制直装
   [ ] K8s Helm Chart
 ▸ [ ] None of the above

 Supplement note (optional, sent with your answer)
   预算上限 5000，下月生效

 ↑↓ move   Space toggle   Enter edit/submit   Esc cancel
```

独占激活态（预设清空、补充区整体 dim）：

```
   [ ] Docker 容器部署              <- dim
   [ ] 二进制直装                   <- dim
 ▸ [x] None of the above   都不合适，我们用裸机自托管

 Supplement note                    <- dim
   预算上限 5000，下月生效            <- dim
```

**焦点与按键**：

- 焦点行 = 预设行 `0..N-1`、「以上都不是」行 `N`、补充说明内容行 `N+1`；补充说明标签行不参与焦点
- Space：「以上都不是」行切换激活（激活即清空预设）；补充说明行忽略（无勾选语义）
- Enter：预设行 = 提交（多选）/ 选中并提交（单选）；「以上都不是」行与补充说明内容行 = 进入输入态
- Esc：输入态返回列表（文本保留）
- dim 行焦点仍可经过，Enter 可再编辑

**输入态**沿用现有模式（列表下方出现提示行 + 输入行），提示词按入口区分：

- 独占入口：` None of the above - your answer (Enter to submit, Esc to back out):`
- 补充入口：` Supplement note (Enter to submit, Esc to back out):`

**校验报错**（grill Q1：拦截点在**离开当前题时**，非整组提交）：commit 当前题时 active 入口文本为空 → 阻止推进，hint 行临时显示错误（如 `! None of the above requires an answer`，静态文案英文），填字或停用后才能走。

**状态字段调整**（`src/tui_state.hpp`，决策⑤ B：结构化对称，删除旧单 "Other" 建模）：

| 字段 | 变化 |
|---|---|
| `ask_custom_answer_selected` | **删除**（单一 "Other" 建模，被双入口取代） |
| `ask_custom_answers` | **删除** |
| `ask_other_input_active` | **删除**（布尔输入态标记，被枚举取代） |
| `ask_exclusive_active` | 新增，独占入口勾选态 |
| `ask_exclusive_text` | 新增，独占入口手填文本 |
| `ask_supplement_text` | 新增，补充入口手填文本（非空即 active） |
| 输入态目标 | 新增枚举 `ask_input_target ∈ {none, supplement, exclusive}`，取代原 `ask_other_input_active` 布尔 |
| 焦点行总数 | `option_count + 2`；`ask_multi_selected` 仍为 `option_count`（仅预设） |

> 删除旧 `ask_custom_*` 字段是破坏性改动，牵连 `main.cpp:6722 commit_current_answer` 与 `tui_ask_channel.cpp` 多处（原 `ask_other_input_active` / `ask_custom_answer_selected` / `ask_custom_answers` 的赋值与清理）。改造后这些位置改为写入 `ask_exclusive_*` / `ask_supplement_text`，并在 `main.cpp` 把双字段填回 `AskUserQuestionAnswer` 再交给 `format_ask_answers`，使 TUI 路径不再走 join 串旁路。

---

## 7. 测试策略

1. **`web/src/lib/questionPicker.test.js`（先写测试，TDD）**——纯逻辑层：
   - 初始答案含 supplement / exclusive 两个入口字段
   - 激活 exclusive → 预设清空、supplement 停用且 text 保留
   - 激活 supplement → 预设不动、exclusive 停用且 text 保留
   - 停用入口 → text 保留、不进 payload
   - `isQuestionAnswered` 三种判定（预设 / 补充非空 / 独占 active——exclusive active 空文本判已作答）
   - `buildQuestionAnswerPayload` 只输出 **active 入口**的文本字段；**exclusive active 时 payload 不含 supplement_text**（即使本地有保留旧文本）——grill Q2
2. **`format_single_answer` 纯函数单测（C++）**——最关键的一层：仅预设 / 预设+补充 / 仅补充 / 独占 / 空组合；标记词与分隔符精确断言；`format_ask_answers` 外层行为不变（回归）
3. **`tests/tui/ask_question_overlay_test.cpp`**——现状只在测试名/注释里出现 “Other”、无字符串断言，改为新增布局断言：可聚焦选项行数 = `option_count + 2`（预设 + 以上都不是 + 补充说明内容行，标签行不可聚焦）、dim 标志、焦点行索引
4. **`tests/tool/ask_overlay_input_test.cpp`**——输入态在两个入口间的切换与文本保留

---

## 8. 分批计划与 OpenSpec

按“可分批实现”的要求拆两批，落在同一个 OpenSpec change（`openspec/changes/ask-user-question-dual-entry/`），`tasks.md` 按批次分组，完成一项勾一项：

**批次 1 · 痛点主战场（桌面端）**

1. 新增 `format_single_answer` 纯函数 + 单测，替换 TUI commit 与 daemon 解析（`routes_ws.cpp:377`）两处内联拼装（grill W1/W3：`parse_async_response` 不存在；`format_ask_answers` 保持原样只包外层）
2. 协议字段改为 `supplement_text` / `exclusive_text`；payload 按 active 过滤（exclusive active 时丢弃 supplement_text）+ daemon 解析层防御性校验
3. 工具 schema（**仅一处**，`ask_user_question_tool.cpp:371`，grill W2）description 改写
4. `ask_user_question_prompter.hpp` 注释 + `docs/daemon-api.md` 更新
5. `questionPicker.js` 纯逻辑改造（先写测试，含 payload active 过滤、`isQuestionAnswered` 判定）+ `QuestionPicker.jsx` UI 落地方案 C 六状态与离开校验（next/提交就地拦截）
6. `pnpm test` + `pnpm build`

**批次 2 · TUI 对齐**

7. `tui_state.hpp` **删除旧 `ask_custom_*` 三字段**、新增 `ask_exclusive_active` / `ask_exclusive_text` / `ask_supplement_text` / 枚举 `ask_input_target`（取代 `ask_other_input_active`）；`main.cpp` 与 `tui_ask_channel.cpp` 相应赋值/清理处改为填 `AskUserQuestionAnswer` 双字段、经 `format_single_answer` 构造答案串（取代 join 串旁路）
8. `ask_question_overlay.cpp` 布局与 dim 渲染（静态文案英文）
9. `main.cpp` 事件处理（焦点范围、Space/Enter/Esc 语义、**离开当前题时校验**——commit 拦截 active 空文本）
10. overlay 与输入态测试更新

---

## 9. 已确认的决策记录

| # | 决策 | 结论 |
|---|---|---|
| 1 | 语义模型 | 双入口（我要补充 + 以上都不是），非单一独占 |
| 2 | 互斥矩阵 | 预设×补充 不互斥；预设×独占 互斥；补充×独占 互斥 |
| 3 | 补充可否独立 | **可以**——不选预设也能只提交补充文本（与独占产物相同，仅入口意图不同） |
| 4 | 反悔/恢复 | **数据永不自动清除**：退出入口时文本保留置灰，重新激活即恢复；预设勾选不复活 |
| 5 | 空文本 | 激活入口文本为空 → 阻止提交 |
| 6 | 意图传达 | 方案 A：**AI 完全知情**，双字段协议 |
| 7 | GUI 布局 | 方案 C · 主输入区（补充为题目下方常驻框，非空即激活） |
| 8 | TUI 布局 | 方案 2 · 分区式（与 GUI 方案 C 心智一致） |
| 9 | 两端行为 | 桌面与终端统一 |
| 10 | 拼装统一点（v0.9.x 审计） | `format_ask_answers` 已是 TUI/daemon 共用统一入口；daemon 解析内联 `routes_ws.cpp:377`，无独立 `parse_async_response`；工具 schema 仅一处（`ask_user_question_tool.cpp:371`） |
| 11 | TUI 双入口落地（决策⑤） | **B：结构化对称**——TUI 也产 `supplement_text`/`exclusive_text` 喂 `format_ask_answers`，删除旧 `ask_custom_*` 字段，不再走 `ask_result_answers` join 串旁路 |
| 12 | 协议注释行号 | `prompter.hpp` 实际 35-41 行（非初稿 33-35） |
| 13 | 空文本拦截时机（grill Q1） | **离开当前题时就地拦截**：active 入口文本空 → 阻止离开（TUI 回车 / GUI next·提交），填字或停用后才能走；原规则 3「允许跳过、最后统一拦」作废（TUI 无整组提交时点）；GUI 最终提交保留全量校验兜底，无需跨题跳转 UI |
| 14 | payload 过滤（grill Q2） | 按 **active 语义**过滤：exclusive active 时丢弃 supplement_text（即使本地有保留旧文本）；daemon 双非空兜底仅防外部直连 API |
| 15 | 文案语言（grill Q3） | 产物标记词**固定中文**（单点常量）；TUI 新增入口标签/提示/报错**沿用英文**（与全英文 overlay 一致）；GUI 维持中文；接受终端字面不一致 |
| 16 | 拼装点修正（grill W3） | 纯函数 `format_single_answer` 落在**每题答案串构造层**；`format_ask_answers` 保持原样（只做 `"Q"="A"` 引号包裹，不加工答案内容） |
