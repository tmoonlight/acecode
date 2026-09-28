<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

# Design: redesign-tui-ask-user-question

## Context

当前 TUI 已通过 `ToolContext::ask_user_questions` 接入共享 `AskUserQuestion` 工具；`src/tui/tui_ask_channel.cpp` 负责把 JSON payload 写入 `TuiState`、等待结果并返回 JSON。overlay 的排版和滚动数学集中在 `src/tui/ask_question_overlay.*`，而题目选择、Other 输入、导航、鼠标和提交事件仍散落在 `TuiState` 与 `main.cpp`。现有实现虽有多题、滚动、鼠标和 timeout 基础能力，但模型不统一，且 `Other...` 复用普通 prompt 输入状态。

此变更主要替换 TUI 适配路径，并补充 AskUserQuestion 的跨端题目数量配置。公开工具参数的字段、共享异步 channel 形状、daemon/Web/Desktop UI 和持久化会话消息不改变；题目数组上限由固定值改为 `ask.max_questions`，默认 10、合法范围 1–50。TUI、daemon、Web、Desktop 背后的 daemon 以及 headless 均使用同一配置值。

## Goals / Non-Goals

**Goals:**

- 将 TUI 问答交互从 `main.cpp` / `TuiState` 中提炼为纯 C++、可单测的深模块。
- 保持单题快速完成，并为 2–`ask.max_questions` 题请求提供自动前进、回看和只读汇总提交；当配置上限为 1 时，仅允许单题快问模式。
- 使自定义答案、键盘、鼠标、滚动、超时和选中反馈的优先级明确、非阻塞且可测试。
- 保持既有 FIFO 请求队列和 `ToolContext::ask_user_questions` 异步边界。
- 不因 TUI 视觉重设计改变模型可见的成功/取消基本语义。

**Non-Goals:**

- 不修改 `AskUserQuestion` 的选项数限制或 `multiSelect` 语义；题目数上限由 `ask.max_questions` 控制，默认 10。单次请求超过有效上限时工具返回明确错误，由模型自行拆成多次调用；系统不自动拆分，也不自动合并多次问答。
- 不重做 daemon、Web 或 Desktop 问答界面，也不引入跨端新协议。
- 不把 AskUserQuestion 扩展为纯文本题或通用表单。
- 不保留旧、新两套 TUI 问答路径的运行时开关。

## Architecture

### D1. 纯模型、布局、FTXUI 适配三层

新增集中在 `src/tui/` 的聚焦类型：

1. **问答控制器**：拥有题目页/汇总页、每题答案变体、焦点、编辑状态、逻辑滚动偏移与语义状态转换；不依赖 FTXUI、时钟、剪贴板或终端坐标。
2. **UTF-8 行内编辑器**：纯 C++ 核心，拥有文本、字节安全的 codepoint 边界、光标、选区、逐行移动、删除和替换；控制器组合该类型，而不复用普通 composer 状态。
3. **布局与命中计算**：以只读控制器快照和终端字符单元格尺寸生成可绘制行、双列宽度、折行、视口、滚动条和命中矩形；不改变控制器状态。
4. **FTXUI 适配器**：将 FTXUI 键盘/鼠标/滚轮/resize/tick 规范化为领域事件，调用控制器，执行语义效果（channel 回调、剪贴板、重绘、toast 和 deadline 调度），并把布局快照绘制为 FTXUI 元素。

`main.cpp` 仅保留 active overlay 的路由、FTXUI event loop 接线和渲染挂接，不再拥有问答业务状态机。

### D2. 问答会话对象与 channel 适配

每个 active 请求由一个 TUI 问答会话对象承载：

- 不可变请求：解析后的问题、子任务来源展示文本、已校验配置；
- 控制器；
- 可选固定 timeout deadline；
- 可选预设项选中反馈 deadline；
- 双击适配器状态（同一命中区域和 500ms 窗口）；
- 临时 toast 状态。

`ask_via_tui_overlay` 继续是唯一阻塞适配器：它入队并等待会话产生结构化完成效果，再按既有 JSON response 契约返回 `{cancelled, timed_out, answers}`。FIFO 仍由 TUI overlay 占用协调保证，同一时间只激活一个会话。会话完成、取消或超时后销毁，随后激活下一个请求。

控制器不直接调用 channel 或系统服务。它消费领域事件并产生效果，例如 `RequestRedraw`、`SubmitCurrent`、`Complete`、`Cancel`、`CopyText`、`ShowToast`、`StartSelectionFeedback`。适配器执行效果并在必要时向控制器反馈，如剪贴板成功/失败或 deadline 到期。

### D3. 显式答案状态与结果映射

每题使用明确的答案变体而非“索引 + 若干布尔值”推断：

- `NotAnswered`；
- 预设选择集合（保持显示顺序）；
- 自定义草稿及其是否激活；
- timeout 自动选择标记。

单选题的有效答案始终只有一个：激活自定义答案会清除预设；选择预设会取消自定义激活但保留草稿。多选题的预设集合与激活的非空自定义补充可共存。提交时按显示顺序输出预设 label，最后追加有效自定义文本；没有有效值则映射为 `Not answered`。

会话完成后，TUI 适配器构造现有 response JSON。控制器返回的“用户取消”不伪装为空答案，适配器继续映射到既有失败路径及明确取消文案。普通、未作答和 timeout 自动选择的结构化信息只在 TUI 本地结果 metadata/转录适配中使用，由适配器生成紧凑 Q/A 卡片；控制器不格式化转录或写持久化。

### D4. 事件优先级、编辑与输入

适配层将原始事件转换为：导航、选择、提交、字符输入、编辑命令、复制、滚动、点击、双击、拖选、resize、feedback-deadline、timeout-deadline 等领域事件。

控制器按当前模式处理：

- **预设项焦点**：单选/多选的 Space、Enter 与数字遵循需求文档；预设项上的普通字符进入自定义编辑。`j/k` 导航，`y` 请求复制 `<label> <description>`。
- **自定义项/编辑态**：普通字符（包括 `j/k/y`）输入编辑器；方向键和 Shift+方向键优先移动/扩展编辑选区；Ctrl+Enter 换行，Ctrl+X/Ctrl+V 编辑，Ctrl+C 留给全局取消。
- **题目页非编辑态**：上下移动行焦点，左右/Tab 仅在多题间导航，PageUp/PageDown 只滚动。
- **汇总页**：Enter 提交；Esc 取消；左/Shift+Tab 到最后题，右/Tab 到第一题；上下不响应。

非编辑题目页的第一次 Esc 先执行既定局部清除并记录时间；一秒内第二次 Esc 取消整个问答。编辑态第一次 Esc 仅按编辑规则退出；第二次才可触发全局取消。反馈锁定期间仅响应 Shift+X、Ctrl+C、汇总页 Esc 与双击 Esc 的全局取消，其余输入忽略。

预设单击切换选中；同一命中区域 500ms 内双击由适配器判定并派发“确保选中后提交”。自定义项双击等同连续两次单击，不提交。布局层提供字符单元格命中矩形；适配器转换坐标，控制器不接触原始坐标。右键仅当编辑器存在选区时请求复制选中文本。

### D5. 布局、视口与安全降级

布局层从控制器只读快照生成：头部、题干、预设项、自定义行、汇总项、动态帮助和超时提示。预设项使用编号列、标记列、标题列、说明列四个固定槽位，标题列用主文字色不加粗、说明列用弱化色不加粗；说明仅在自身列中折行。推荐项由工具解析层从现有 label 的 `[Recommended]` 约定解析为显式领域标志，控制器不扫描展示文本。

顶层瞬时状态行（模型切换、更新提示等）不属于面板内容，只出现在顶部标题区与底部状态区；把它塞进面板行既会误导用户，也会让面板高度随无关事件变化。面板绘制前对其矩形整体做擦除（字符层面，而不只是背景色），否则聊天内容会从面板未写字的格子透出。绘制集中在 `src/tui/ask_question_panel.*`：颜色由调用方注入语义值，因此渲染结果可以用 FTXUI `Screen` 逐格断言。

布局以当前终端行/列动态计算可视区域；`question_min_visible_rows` 是最小可见内容行数的目标值，而不是强制占用的固定高度或像素阈值。实际终端空间不足时按当前视口降级，不强行撑大 overlay。内容超过视口时才渲染可拖动滚动条。控制器持有逻辑行偏移，布局计算最大偏移与最小滚动调整以确保焦点、编辑光标或选择范围可见；适配器将滚轮、PageUp/PageDown 与滚动条拖动转换为滚动事件。汇总页不响应键盘上下，但支持滚轮和拖动条。

resize 后重新计算布局，保留答案、焦点、编辑器光标/选区和逻辑偏移，再将偏移钳制到新范围。终端极窄时优先保留编号、选择标记和标题；说明列压缩、折行或省略。低于安全最小尺寸显示短暂“终端过窄”提示并暂停题目交互，恢复空间后继续原状态。

### D6. 时间与反馈

控制器不读取系统时间。外层会话以 `steady_clock` 维护：

- 请求展示时一次性建立 timeout deadline；用户任何操作均不重置它；
- 预设项提交产生可选反馈效果；适配器设置一次 selection-feedback deadline，0ms 直接推进；
- FTXUI event loop 的非阻塞 tick 在 deadline 到达时派发事件，绝不创建后台线程。

timeout 策略未启用时没有 deadline。启用时布局显示实时倒计时。到期逐题收卷：保留有效预设或完成的自定义答案；激活且非空的草稿作为用户答案；激活但为空的自定义项为 `Not answered`；无有效答案时仅自动选择该题第一个显式 Recommended 预设项，没有推荐项则保持 `Not answered`。自动选择标记供本地转录显示 `[Auto-selected]`。

### D7. 配置

在配置加载层新增并校验：

```toml
tui.question_min_visible_rows = 4        # clamp [2, 12]
tui.question_selection_feedback_ms = 200 # clamp [0, 1000]
```

非法值在加载时钳制到默认值或边界，并使用现有配置警告日志输出一次诊断。会话对象只接受已校验的强类型配置，不重复分散校验。保存遵循现有 sparse-on-write 约定；相关配置文档同步更新。

## Testing Strategy

纯 controller/editor/layout API 加入 `acecode_testable`，通过固定请求、领域事件、尺寸和人工推进的 deadline 测试，不依赖 FTXUI、真实时钟或系统剪贴板。

覆盖：

1. 单题快问、多题自动推进、汇总导航与提交；
2. 单选/多选、自定义草稿、互斥与答案顺序；
3. 编辑器 UTF-8、光标、选区、多行、粘贴/剪切与 Esc；
4. 键盘优先级、数字快捷键、局部 Esc 与双击 Esc、反馈锁定；
5. timeout 收卷、推荐项缺失、反馈 deadline 与固定 timeout；
6. 双列折行、窄终端降级、视口、滚动范围、焦点可见性、命中区域和 resize；
7. response/转录适配、取消失败语义、timeout metadata 与 FIFO 生命周期；
8. 少量 TUI 集成测试：FTXUI 原始事件规范化、channel 等待/唤醒、鼠标双击与 clipboard 成败 toast 接线。

## Migration Plan

1. 先引入纯模块及其单元测试，并接入 `acecode_testable`。
2. 在 `tui_ask_channel` 建立会话对象和适配器，完成 response 映射及测试。
3. 将 `main.cpp` 的 ask 事件/渲染委托给适配器，替换旧 `TuiState` ask 字段和分支。
4. 删除旧 overlay 的重复状态及 `Other...` composer 耦合路径；不保留运行时切换开关。
5. 运行 focused tests、完整 C++ 测试和质量检查；手动验证窄终端、鼠标、timeout 和队列。

## Risks / Trade-offs

- 行内 UTF-8 编辑、拖选及字符单元格命中复杂度高；以独立纯核心和布局 seam 控制风险。
- 全量替换减少双路径维护，但缺少运行时回退；通过迁移分层、完整状态矩阵与 focused integration tests 降低风险。
- FTXUI tick/鼠标能力可能存在平台差异；适配层集中差异并使核心事件可 deterministic 测试。
- 推荐标记是既有 label 文本约定；解析层一次性形成显式标志，防止业务逻辑和渲染字符串耦合。

## Open Questions

（无。技术设计与推荐决策已由用户确认；后续未特别提出的决策按推荐方案执行。）
