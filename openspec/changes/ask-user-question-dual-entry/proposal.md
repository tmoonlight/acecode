# Proposal: ask-user-question-dual-entry

> 设计基线：`docs/superpowers/specs/2026-09-06-askuserquestion-dual-entry-design.md`（已评审通过，经 grill with doc 修订，决策表 1-16）。本 change 的详细语义、状态模型、协议、两端 UI、测试与分批均以该文档为准，此处只做实施级摘要。

## Why

当前 AskUserQuestion 的 UI 自动追加单一「自定义项」，只有"追加"语义——手填文本必须与已勾选的预设共存提交（daemon 侧以 `", "` 拼合 `selected` 与 `custom_text`），最终答案混进用户并不认可的预设。更糟的是两条路径行为不一致：TUI 同步路径提交自定义文本时直接覆盖答案、静默丢弃已勾选项；daemon 路径则合并。

需把自定义项拆成两个语义明确的入口，并让模型感知用户意图：**「我要补充」**（追加式，与预设共存）与**「以上都不是」**（独占式，作废全部预设）。

## What Changes

### 语义（决策 1-6、grill Q1-Q2）

- 双入口互斥矩阵：预设×补充 不互斥；预设×独占 互斥（激活即清空预设勾选）；补充×独占 互斥（激活一方停用另一方，**文本保留置灰**）。
- 补充可独立存在（不选预设也能只提交补充文本）。数据永不自动清除：退出入口文本保留置灰、重新激活即恢复；预设勾选不复活。
- 空文本校验：**离开当前题时就地拦截**（TUI 回车 / GUI 下一步与提交）——任一 active 入口文本为空则阻止离开并就地提示。GUI 最终提交保留全量校验兜底。原「允许跳过、最后统一拦」作废（TUI 无整组提交时点）。
- payload 按 **active 语义**过滤：exclusive active 时丢弃 supplement_text（即使本地留有非空旧文本）。

### 协议与拼装（决策 10-12、16、grill W1-W3/Q3）

- `AskUserQuestionAnswer.custom_text` → `supplement_text` + `exclusive_text`（`src/session/ask_user_question_prompter.hpp`，注释同步改写互斥规则与"非空即选中"）。`docs/daemon-api.md` 同步。
- 新增纯函数 `format_single_answer(selected_labels, supplement_text, exclusive_text)` → 带标记答案串：`预设A, 预设B; 补充: x` / `补充: x` / `以上都不是: x`。标记词中文单点常量；分隔符 ASCII（`; `、`: `）。TUI commit 与 daemon 解析两处内联拼装收敛到它；`format_ask_answers` 保持原样（只做 `"Q"="A"` 引号包裹）。
- daemon 解析防御性兜底：两文本同时非空（外部直连 API 违规）时以 `exclusive_text` 为准并记日志。
- 工具 schema `options` description 改写（仅一处 `ask_user_question_tool.cpp:371`）：告知模型 UI 自动追加两个入口，无需也不应写进 options。

### React GUI（决策 7，方案 C · 主输入区）

- 「以上都不是」= 选项列表内行：勾选框 + 标签 + 行内输入框，暖色强调独占性。
- 「补充说明」= 题目下方常驻文字域，无勾选框、**打字即生效**（非空即 active）。
- 置灰态：被停用入口的输入框 dim 保留文本并标注。C 规则：打字即生效 / 勾独占→补充置灰 / 点回置灰框→自动恢复并取消独占 / 校验在离开当前题时。
- `questionPicker.js` 纯逻辑改造（先写测试）：双入口状态字段、互斥切换、`isQuestionAnswered` 三判定（exclusive active 空文本判已作答）、`buildQuestionAnswerPayload` 按 active 过滤。
- 六个状态走查（用户确认）：默认 / 预设+补充 / 独占激活 / 反悔改选预设 / 校验报错（红框+阻止离开）/ 单选题形态。

### TUI（决策 8、11，方案 2 · 分区式；grill Q3 英文静态文案）

- `TuiState`：删除 `ask_custom_answer_selected` / `ask_custom_answers` / `ask_other_input_active`；新增 `ask_exclusive_active` / `ask_exclusive_text` / `ask_supplement_text` / 枚举 `ask_input_target ∈ {none, supplement, exclusive}`。
- 焦点行 = 预设 0..N-1 + 独占行 N + 补充内容行 N+1（标签行不可聚焦）；`ask_multi_selected` 仍仅覆盖预设。
- 静态文案英文：入口 `None of the above` / `Supplement note`（与全英文 overlay 一致）；产物标记词仍中文（单点常量），接受字面不一致。
- 离开当前题校验：commit 时 active 入口文本为空 → 阻止推进，hint 行报错，填字或停用才能走。
- 答案构造：填 `AskUserQuestionAnswer` 双字段 → `format_single_answer`，取代 `ask_result_answers` join 串旁路（`tui_ask_channel.cpp:57` 硬编码空 `custom_text` 根因）。

## Capabilities

### New Capabilities

- `ask-user-question-dual-entry`: AskUserQuestion 双入口（我要补充 / 以上都不是）——状态模型与互斥规则、空文本离开校验、双字段协议与格式单点常量、payload active 过滤、GUI 方案 C、TUI 方案 2、两端行为统一。

### Modified Capabilities

（无 —— 现有 spec 目录下无 AskUserQuestion UI 结构相关 spec。）

## Impact

- **协议/头**: `src/session/ask_user_question_prompter.hpp`（`AskUserQuestionAnswer` 结构、注释）
- **拼装**: 新增 `format_single_answer`（`src/tool/`，纯函数，进 `acecode_testable`）+ 单测；`format_ask_answers` 不动
- **工具 schema**: `src/tool/ask_user_question_tool.cpp:371` `question_schema` description
- **daemon 解析**: `src/web/routes/routes_ws.cpp:377` 内联解析改双字段 + 兜底
- **前端**: `web/src/lib/questionPicker.js`（纯逻辑 + `questionPicker.test.js` TDD）、`web/src/components/QuestionPicker.jsx`（方案 C UI）
- **TUI**: `src/tui_state.hpp`（字段增删）、`src/main.cpp`（commit/事件/校验）、`src/tui/tui_ask_channel.cpp`、`src/tui/ask_question_overlay.cpp`
- **文档**: `docs/daemon-api.md:3154`
- **测试**: React 纯逻辑、`format_single_answer` C++ 单测、`tests/tui/ask_question_overlay_test.cpp` 布局断言、`tests/tool/ask_overlay_input_test.cpp` 输入态切换

## 分批

批次 1 桌面端（协议 + 拼装 + GUI）：1.1-1.7。批次 2 TUI：2.1-2.4。见 `tasks.md`。
