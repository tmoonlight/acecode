# ACECode 文档索引

本目录是 ACECode 设计与交付文档的入口。产品行为契约以 OpenSpec 为准，
`docs/` 记录跨模块设计、实现边界和交付计划。

## 设计范围

| 范围 | 入口 | 说明 |
|---|---|---|
| Web/Desktop 聊天 | [web-chat/README.md](web-chat/README.md) | Transcript 状态所有权、投影、窗口化、滚动与交互边界 |
| Desktop Shell | [desktop-shell/design.md](desktop-shell/design.md) | 原生窗口、WebView 和多工作区外壳 |
| Agent Browser | [agent-browser.md](agent-browser.md) | 浏览器面板、宿主桥接和网络诊断 |
| Daemon API | [daemon-api.md](daemon-api.md) | HTTP、WebSocket 与 session 事件协议 |
| Hooks | [hooks.md](hooks.md) | Hook 生命周期和配置 |
| Skills | [skills.md](skills.md) | Skill 发现、加载和运行契约 |
| Localization | [localization.md](localization.md) | Web/Desktop 文案来源和本地化流程 |
| User Manual | [user-manual.md](user-manual.md) | 面向用户的功能说明 |

## 交付计划

- [plan/README.md](plan/README.md)：计划目录约定。
- [plan/analysis/](plan/analysis/)：问题分析与任务拆分。
- [plan/tasks/](plan/tasks/)：可独立验证的交付任务。
- [plan/reviews/](plan/reviews/)：独立审查结论。
- [plan/backlog.md](plan/backlog.md)：非阻塞后续事项。

## 复盘

- [postmortem-file-link-preview.md](postmortem-file-link-preview.md)：AI 生成的文件链接点不开 —— 渲染、编码、路径、数据传递四层缺陷的定位过程与回归防线。
