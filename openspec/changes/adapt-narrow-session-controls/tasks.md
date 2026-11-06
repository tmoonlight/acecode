## Implementation

- [x] 1. 标题栏在空间不足时按优先级单行省略，兼顾重命名。
- [x] 2. 输入框工具栏按实际宽度压缩并保留操作。
- [x] 3. 底部状态按可用宽度省略，覆盖待办、变更和组合状态。
- [x] 4. 验证真实浏览器中的窄栏、加宽恢复和正文换行，运行 Web 测试、构建与 OpenSpec 严格校验。

## 验证记录

- Edge / Chromium 的完整 App 在 480 CSS px、右侧文件面板展开时，工作目录名单行省略，标题栏高度与 1920 px 时一致，消息正文实际排成多行。
- 在同一窗口开合文件面板后，统计随栏宽收起和恢复。实际点击“查看变更”切换到变更页签，关闭摘要后单独的待办步数可见。完整 `test-responsive-sidebars.mjs` 通过。
- `test-composer-model-label.mjs` 覆盖模型名省略和恢复，以及 168 px 容器内所有工具按钮的点击命中、权限和思考深度菜单。`test-session-title-animation.mjs` 覆盖共享标题、就地重命名及动画。
- `pnpm test`、`pnpm build`、`openspec validate adapt-narrow-session-controls --strict` 及差异检查通过。验证使用浏览器中的桌面标题栏，未重建或启动原生桌面包。
