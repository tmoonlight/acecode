<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

`projectCollapsedTranscriptItems` 负责工具配对、特殊分组、工具段与完成轮次折叠；子代理通过 `projectSubagentTranscriptItems` 复用。`TranscriptItems` 递归渲染两种会话。外观保存已有顺序写入、回滚和 Desktop 首帧注入机制。

## Goals / Non-Goals

- 新开关默认 true，旧配置缺字段保持现有效果；false 在刷新、重启后保留。
- 主会话与子代理使用同一投影策略；切换后立即重投影，历史与实时消息一致。
- 关闭只保留工具详情的折叠，所有非工具正文和分组内容直接显示。
- 不更改后端历史、工具执行、上下文压缩行为及长会话渐进加载。

## Decisions

1. 以 `web_ui.message_auto_collapse` 为持久化字段，前端使用 `messageAutoCollapse`，复用现有完整外观快照的保存/失败回滚。
2. 在共享投影函数分支跳过完成轮次与工具段汇总，继续使用工具标准化与完成总结生成；实时空状态仍保留不可折叠的状态行。
3. 向主会话、子代理和共享渲染组件传递同一布尔值。图片/子代理组保留语义与跳转入口，但关闭折叠交互；系统通知正文直接展开。
4. 设置复用主题 token 与现有 Toggle。一级标题“会话”可加粗，选项标题字重 400；补齐英文文案。

## Risks / Trade-offs

关闭后同一轮可见行数增加，保留既有 transcript 窗口避免一次渲染全部历史。切换不修改原始消息，重新开启恢复已有折叠路径。测试覆盖工具包装去重、完成总结、运行提示及子代理只读过滤，防止简单绕过投影造成语义退化。
