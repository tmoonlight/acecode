## Why

中文回复中的 `**建议：**共享` 等写法在现有 CommonMark 标点边界规则下直接显示星号。用户希望 ACECode 正常显示这类中文加粗，同时保留已有 Markdown 写法的解析结果。

## What Changes

- 在 WebUI 共享 Markdown 渲染器接入 `markdown-it-cjk-friendly`，兼容中日韩文字与标点相邻的强调标记。
- 覆盖完整、行内和流式分块渲染，并验证转义、代码与已有强调边界不受破坏。
- 更新依赖锁文件及 Web 文档，记录 Windows 本机验证结果。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `webui-rich-rendering`：补充中日韩文字中的强调兼容规则和共享渲染入口一致性。

## Impact

- 涉及 `web/src/lib/markdown.js`、相关测试、`web/package.json`、`web/pnpm-lock.yaml` 和 `web/README.md`。
- 新增 `markdown-it-cjk-friendly` 依赖；沿用现有 `markdown-it`。
- 作用于使用该共享渲染器的 Web/Desktop 内容，不修改会话原文、协议、TUI 解析器或独立的可视化编辑器。
