## 背景

`DesktopContextMenu` 已经是会话、工作区、文件、预览、变更、消息、工具输出和附件的统一对象菜单。当前组件通过 `isDesktopShell() || isWebappCompat()` 决定是否接管 `contextmenu`；普通浏览器因此完全绕过这套对象识别，只能获得浏览器原生菜单。

菜单中的大多数动作由前端状态、CustomEvent 或通用 daemon API 完成，可以在 Web 中运行。真正依赖宿主能力的动作目前集中为资源管理器打开/定位、需要原生目录选择器的会话导出及 Desktop DevTools 检查；普通 Web 不应展示或执行它们。Edge WebApp 兼容模式虽然没有 WebView bridge，但其 Desktop 管理的 daemon 提供既有 REST/原生选择器回退，因此继续视为具备这些宿主能力。

## 目标 / 非目标

**目标：**

- 普通浏览器 Web 与 Desktop 共用唯一的全局对象感知右键菜单。
- 在菜单模型层按运行能力移除 native-only 动作，并在执行层增加防御性拒绝。
- 过滤后重新计算分组分隔线，保持菜单顺序和视觉结构稳定。
- 保留控制台终端自己的右键处理、现有确认语义和 Desktop/WebApp 行为。

**非目标：**

- 不为普通 Web 新增资源管理器、DevTools 或其他原生桥能力。
- 不改变 daemon 路由、Desktop bridge、TUI 或浏览器安全策略。
- 不重构各业务组件的对象元数据或事件处理器。

## 决策

### 1. 解除全局运行模式门控

普通 Web 的 `contextmenu` 将与 Desktop 一样进入 `DesktopContextMenu`；`.ace-console-term` 仍在捕获处理之前直接放行，由 `ConsoleDock` 保持 VS Code 风格菜单。

备选方案是在各 Web 组件中分别实现本地菜单。该方案会重复目标识别、定位、关闭和操作分发，并重新产生已被共享菜单解决的 DRY 问题，因此不采用。

### 2. 在动作模型层集中标记并过滤 native-only 动作

`desktopContextMenu.js` 维护 native-only 动作集合，至少包含：

- `OPEN_IN_EXPLORER`
- `LOCATE_FILE`
- `EXPORT_SESSION`
- `INSPECT`

菜单构建器接收运行能力参数，在调用 `withMenuSeparators` 之前过滤这些动作。这样所有会话、工作区、文件、预览标签、变更和附件入口共享同一规则，且过滤不会留下错误的分隔线。

备选方案是在每个 `addAction` 调用点增加条件。该方案容易遗漏新的入口，也无法保证未来新增 native 动作被统一处理，因此不采用。

### 3. 运行模式只决定 native 能力，不再决定菜单是否存在

Desktop Shell 与 Edge WebApp 兼容模式继续允许既有 native 动作；普通浏览器将 `allowNativeActions` 设为 false。WebApp 的“在资源管理器中打开”继续使用现有 REST 回退，不改变其语义。

执行函数在真正分派前再次检查 native-only 动作。正常情况下菜单模型已将其移除；该检查用于防止旧菜单状态、手工事件或后续调用路径绕过展示层。

### 4. Web 可执行动作保持原样

复制、粘贴、全选、会话操作、文件预览/上下文、变更操作、消息 fork、工具折叠、附件预览/移除及 Mermaid 导出继续使用既有前端、剪贴板或 API 路径。浏览器权限拒绝仍走现有错误反馈，不将具有 Web 回退的动作误判为 native-only。

## 风险 / 权衡

- [风险] native 动作分类遗漏会在 Web 中露出无效菜单项。→ 使用集中集合、普通 Web 组合测试和组件架构测试约束调用参数。
- [风险] 过滤动作后分隔线位置异常。→ 先过滤再调用现有 `withMenuSeparators`，并对顺序进行断言。
- [风险] 普通输入框不再显示浏览器原生菜单。→ ACECode 菜单保留全选、复制、粘贴和剪切；继续依赖现有剪贴板错误处理。
- [风险] WebApp 被误当成普通 Web 而失去文件管理器动作。→ 运行能力继续使用 `isDesktopShell() || isWebappCompat()` 计算并添加回归测试。

## 迁移计划

该变更只有前端行为变化，不需要数据迁移。部署新 Web 资源后立即生效；回滚时恢复普通浏览器门控并移除能力过滤参数即可。

## 待定问题

无。
