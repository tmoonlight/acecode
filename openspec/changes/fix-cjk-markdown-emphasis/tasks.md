## 1. 接入与回归

- [x] 1.1 接入固定版本的插件、更新锁文件及 Web 文档，用共享渲染器确认 `**建议：**共享` 正常加粗。
- [x] 1.2 增加中日韩强调、已有标点边界、字面代码、转义与流式回归用例，纳入测试入口并通过定向测试。

## 2. 综合验证

- [x] 2.1 完成全量 `pnpm test`、`pnpm build`、浏览器渲染验证、OpenSpec 严格验证及 `git diff --check`，在本文件记录范围与结果。

## 验证记录

- Windows 本机，2026-10-03；Node 22.22.0、pnpm 10.32.1。
- 定向执行 `node src/lib/markdownCjkEmphasis.test.js` 与 `node src/lib/markdownBlocks.test.js`：19 项新增回归和 7 项既有分块测试通过。
- `web/` 中执行 `pnpm test`：退出码 0，输出 2,977 条 `[pass]`。
- `web/` 中执行 `pnpm build`：通过；产物扫描 4,498 个正则字面量，未发现旧版 WebKit 不兼容的后行断言。
- Headless Edge 使用真实共享渲染器打包与构建 CSS，渲染用户提供会话的原始片段：11 项检查通过，包括六个列表标签与“建议：”加粗、无残留星号、代码/标题保留、分块一致性及中日韩兼容；截图已目视核对。
- 仓库根目录执行 `openspec validate fix-cjk-markdown-emphasis --strict` 与 `git diff --check`：通过。
- 本次是共享 Web 渲染器的隔离浏览器验证；未重建或替换运行中的 Desktop/daemon，也未执行 TUI 或跨端实机验收。
