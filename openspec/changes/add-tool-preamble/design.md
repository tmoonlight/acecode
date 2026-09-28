<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

# Design: add-tool-preamble

## 业界怎么做(调研结论)

| 产品 / 资料 | 状态行文案来源 | 机制 |
|---|---|---|
| Codex(TUI / 桌面) | OpenAI Responses `reasoning.summary` 的首行 `**加粗**` | `chatwidget.rs::extract_first_bold` 抠出当状态行;拿不到就是 "Working" |
| Codex / OpenAI 提示指南 | 模型写的 preamble | 工具调用前一两句**普通用户可见消息**,按阶段而不是按调用,留在对话里 |
| agentpatterns.ai「Tool Preamble」 | 同上 | 两种落地:系统提示(模型负载大时会忽略)/ harness 注入(确定性但通用);界面已经可视化工具执行时不需要 |
| Gemini CLI | thought summary 的 `**Subject**` | `parseThought`,优先级 shell 等待 > subject > 阶段短语 |
| Cursor / Copilot | 模板 | "Explored (x) tools" 从工具名拼 |

结论:过程说明(preamble 消息)与状态行是两层。状态行要短,来源应当是确定性的(推理摘要标题 / 工具模板),不能指望模型额外输出。

## 四版的取舍

| 版本 | 形态 | 为什么被否 |
|---|---|---|
| v1 | 含工具调用的消息先写一句前言,抠出当批次标题 | 文本先流成气泡、参数再流、批次开始才搬进 loading;按批次拆成一摞标题行 |
| v2 | 每个工具定义注入必填 `preamble` 参数 | 强迫模型每次调用都写一句,体验差;grok 对可选参数几乎不填 |
| v3 | 正文 `<text_preamble type="read|write">` 标签,阶段变化时写 | grok-4.7 不照做(用户会话 20260925-053811-55cc:12 个工具步零标签,全写裸文本);与训练分布相悖 |
| v4(现行) | daemon 生成:推理加粗标题 > 工具现在进行时模板 > 场景文案 | 不依赖模型;loading 短、具体、不带参数 |

## 数据流

```
run_agent_with_input
  reset_activity_for_turn()
  loop:
    call_provider_and_collect
      reset_activity_for_step()                     ← 本步标题 / 已流出工具 / 执行中批次清空
      emit model_waiting ─┐
      reasoning delta ────┼─ 加粗标题 → publish_phase_preamble → emit preamble(force)
      ToolCallDelta ──────┼─ note_planned_tool(index, name) → emit tool_planning
      visible text ───────┘─ 第一次 → emit responding(force)
    resolve_tool_preamble_for_step → {title|template, source, kind}
      记 last_batch_tools_ / current_batch_activity_
    execute_tool_calls → metadata.tool_preamble、tool_start.preamble*、emit tool_running
  reset_activity_for_turn()

emit_agent_progress(phase, label, detail, …)
  concrete = concrete_activity_for_phase(phase)     ← 关闭 / 不替换的 phase 返回空
  if concrete: label = concrete.title; detail = ""; payload.preamble = concrete
               announce_activity(concrete.title) → on_thinking_title(TUI,去重)
```

- **只在发射口替换**:各发射点仍传原来的文案,开关关闭时原样发出,开启时统一换掉,保证没有漏网的「正在推理」。
- **phase → 文案**:`tool_running` 用执行中批次;`tool_planning` 用本步标题,否则按已流出的工具拼模板,都没有就退到场景文案;`model_waiting` / `reasoning` / `preamble` 用本步标题,否则场景文案(回合开头「正在分析你的请求」,之后按上一批工具);`responding` 固定「正在撰写回复」。
- **推理加粗标题只管本步**:每次 provider 调用(含重试)开头清空。推理首句兜底删除:grok 这类只给原始思维链的模型,首句永远是 "The user wants me to…"。
- **模板只看原生工具名**,不看参数:同类计数(读取 / 运行 / 修改 / 写入 / 网页 / 子任务 / 图片可数,搜索 / 查找不计数),两类用「并」,三类及以上前两类加「等」;MCP 与未列出的工具归「调用工具」。
- **kind** 由工具类型定:有写类工具为 write,全是读类为 read,否则空;透传给界面,效果留空。
- **工具行不变**:`tool_start` 的参数、Web `ToolBlock` 运行中的行、TUI 进度头都照常显示参数。
- **系统提示不变**:与开关无关,「# Sharing progress updates」一节保持 v1 之前的原文。

## 工作模式入口

`web/src/lib/workMode.js`:`workModeFromToolPreamble(state)`(enabled → 适合日常工作)、`toolPreambleUpdateForWorkMode(mode)`(→ `{enabled}`)。设置页打开时读一次,点选时 PUT、以响应为准,失败回滚并提示;读取完成前两个卡片禁用。工作模式不单独存,避免界面与 daemon 漂移。

## 已知限制

- 推理摘要不带加粗标题的模型(grok、DeepSeek、Claude)只能用模板与场景文案,不会有模型写的具体描述。
- 模板文案是中文,英文界面显示的也是中文(与原来 daemon 下发的动态阶段文案一致)。
- headless `-p` 模式下开关照常生效,但 stdout 只出最终文本,看不到 loading。
