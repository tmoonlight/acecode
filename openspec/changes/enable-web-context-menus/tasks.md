## 1. 菜单能力模型

- [x] 1.1 在 `desktopContextMenu.js` 集中定义 native-only 动作，并提供可复用的能力判断/过滤 helper。
- [x] 1.2 让菜单构建器按 `allowNativeActions` 过滤动作后再计算分隔线，同时保留默认 Desktop/WebApp 行为。
- [x] 1.3 扩充菜单模型单元测试，覆盖普通 Web 过滤资源管理器、定位、会话导出和检查动作以及 Web 动作顺序。

## 2. 普通 Web 接入

- [x] 2.1 解除 `DesktopContextMenu` 对普通浏览器的全局门控，并继续放行控制台终端自己的右键菜单。
- [x] 2.2 按 Desktop Shell / Edge WebApp / 普通浏览器计算 `allowNativeActions`，将能力传入菜单构建器。
- [x] 2.3 在动作执行边界拒绝普通 Web 的 native-only 动作，并增加组件架构回归测试。

## 3. 验证与收尾

- [x] 3.1 运行右键菜单聚焦测试与前端完整测试。
  - 验证记录：右键菜单聚焦与架构测试通过；完整 `pnpm test` 被本轮开始前已有的模型设置资源计数断言 `185 !== 193` 阻断。
- [x] 3.2 运行 `pnpm build` 和 `pnpm i18n:audit`。
- [x] 3.3 在普通 Web 与 WebApp 兼容模式进行浏览器冒烟，验证菜单启用和 native 动作差异。
- [x] 3.4 运行 `openspec validate enable-web-context-menus --strict` 与 `git diff --check`。
