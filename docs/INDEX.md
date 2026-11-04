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
| Themes | [themes.md](themes.md) | AI 主题创建、图标与标题配色、背景通顶、自定义主题导出与删除、EVA 资源包 |
| Localization | [localization.md](localization.md) | Web/Desktop 文案来源和本地化流程 |
| User Manual | [user-manual.md](user-manual.md) | 面向用户的功能说明 |

## Desktop Shell 专题备忘

- [desktop-shell/multi-workspace.md](desktop-shell/multi-workspace.md)：多工作区与共享 daemon 行为。
- [desktop-shell/macos-frameless-titlebar.md](desktop-shell/macos-frameless-titlebar.md)：macOS 无边框标题栏的安全实现边界。
- [desktop-shell/macos-file-drop.md](desktop-shell/macos-file-drop.md)：macOS Finder 文件拖放的根因、修复架构、平台影响与回归清单。
- [desktop-shell/macos-file-drop-debug.md](desktop-shell/macos-file-drop-debug.md)：macOS 文件拖放的本地构建与实机验证步骤。

## 交付计划

- [plan/README.md](plan/README.md)：计划目录约定。
- [plan/analysis/](plan/analysis/)：问题分析与任务拆分。
- [plan/tasks/](plan/tasks/)：可独立验证的交付任务。
- [plan/reviews/](plan/reviews/)：独立审查结论。
- [plan/backlog.md](plan/backlog.md)：非阻塞后续事项。
- [string-utilities-refactor.md](string-utilities-refactor.md)：字符串工具统一重构方案（**未启动**，含现状盘点与护栏设计）。

## 调研

- [竞品调研报告_完整版_2026-08.md](竞品调研报告_完整版_2026-08.md)：11 产品竞品全景对比（Pi / Claude Code / Codex / Qoder / Trae / Cowork 等），关键论断已逐条对照源码核实，含 Phase 0–3 改进规划。

## 复盘

- [postmortem-file-link-preview.md](postmortem-file-link-preview.md)：AI 生成的文件链接点不开 —— 渲染、编码、路径、数据传递四层缺陷的定位过程与回归防线。

## 归档

- [archive/claude-md-2026-11-04.md](archive/claude-md-2026-11-04.md):根目录 CLAUDE.md 在 2026-11-04 精简前的全文,各子系统的实现记忆、踩过的坑与回归测试清单,只读不更新;当前规则以根目录 [CLAUDE.md](../CLAUDE.md) 为准。
