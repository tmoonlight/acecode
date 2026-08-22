## 为什么

普通浏览器直连 ACECode Web 时被现有门控排除在全局自定义右键菜单之外，导致会话、文件、变更、消息等对象已有的右键操作全部不可达。Web 应复用同一套对象菜单，但不得展示或触发依赖 Desktop 原生桥接的动作。

## 变更内容

- 在普通浏览器 Web 模式中启用 ACECode 全局自定义右键菜单，复用现有对象识别、菜单排序、确认和事件分发逻辑。
- 为菜单项增加运行能力过滤：普通 Web 隐藏“在资源管理器中打开”“在资源管理器中显示”、依赖原生目录选择器的会话导出和原生调试检查等 Desktop/native 动作。
- Desktop Shell 与 Edge WebApp 兼容模式继续保留现有原生能力；控制台终端继续使用自身的右键菜单。
- 增加纯函数和架构回归测试，覆盖普通 Web 启用、native 动作过滤以及 Desktop/WebApp 行为保持。
- 不新增 daemon API、原生桥协议或依赖，不改变现有破坏性操作的确认语义。

## 能力

### 新增能力

- `web-context-menu-actions`：规定普通 Web 启用对象感知的 ACECode 右键菜单，并按运行模式过滤不可用的 native 动作。

### 修改能力

无。

## 影响

- 前端菜单模型：`web/src/lib/desktopContextMenu.js`。
- 前端全局菜单：`web/src/components/DesktopContextMenu.jsx`。
- 前端测试：`web/src/lib/desktopContextMenu.test.js` 及必要的架构测试。
- 不修改 Desktop 原生实现、daemon 路由、会话数据或 TUI 行为。
