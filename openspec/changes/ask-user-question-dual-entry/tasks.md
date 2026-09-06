# Tasks: ask-user-question-dual-entry

> 依据 `docs/superpowers/specs/2026-09-06-askuserquestion-dual-entry-design.md` 第 8 节分批。完成一项勾一项。

## 批次 1 · 桌面端（协议 + 拼装 + GUI）

- [ ] 1.1 新增 `format_single_answer(selected_labels, supplement_text, exclusive_text)` 纯函数（`src/tool/`，标记词中文单点常量）——仅预设 / 预设+补充 / 仅补充 / 独占 / 空组合，精确断言标记词与分隔符；`format_ask_answers` 外层回归（tests/tool/）
- [ ] 1.2 `AskUserQuestionAnswer.custom_text` → `supplement_text` + `exclusive_text`（`ask_user_question_prompter.hpp`）；`routes_ws.cpp:377` 按双字段解析 + 双非空兜底（以 exclusive 为准记日志）+ 经 `format_single_answer` 构造答案串
- [ ] 1.3 工具 schema（仅一处 `ask_user_question_tool.cpp:371`）`options` description：告知模型 UI 自动追加「我要补充」「以上都不是」，无需也不应写进 options
- [ ] 1.4 `docs/daemon-api.md:3154` answers[] 协议字段同步（custom_text → supplement_text / exclusive_text，注明互斥与兜底）
- [ ] 1.5 `questionPicker.js` 纯逻辑 TDD：初始答案含双入口字段；激活 exclusive→清空预设+停用 supplement(text 保留)；激活 supplement→停用 exclusive(text 保留)；停用入口 text 保留不进 payload；`isQuestionAnswered` 三判定（exclusive active 空文本判已作答）；`buildQuestionAnswerPayload` 按 active 过滤（exclusive active 不发 supplement_text）
- [ ] 1.6 `QuestionPicker.jsx` 方案 C UI 六状态 + 离开校验（next/提交拦截空文本、红框报错）
- [ ] 1.7 `pnpm test` + `pnpm build` 通过

## 批次 2 · TUI 对齐

- [ ] 2.1 `tui_state.hpp`：删除 `ask_custom_answer_selected` / `ask_custom_answers` / `ask_other_input_active`；新增 `ask_exclusive_active` / `ask_exclusive_text` / `ask_supplement_text` / 枚举 `ask_input_target ∈ {none, supplement, exclusive}`
- [ ] 2.2 `main.cpp`：commit_current_answer 与事件处理（焦点范围 option_count+2、Space/Enter/Esc 语义、双入口写入、离开当前题校验拦 active 空文本、经 `format_single_answer` 构造答案串取代 join 串旁路）
- [ ] 2.3 `tui_ask_channel.cpp`：不再硬编码空 custom_text，改填双字段；`ask_question_overlay.cpp` 布局与 dim 渲染（分区式、静态英文 None of the above / Supplement note）
- [ ] 2.4 测试更新（overlay 布局断言：可聚焦行 = option_count+2、dim 标志；输入态切换与文本保留）+ 构建 `acecode_unit_tests` + `ctest`
