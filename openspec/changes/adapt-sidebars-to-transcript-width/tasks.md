## 1. 响应式决策

- [x] 1.1 实现 transcript 优先的纯布局决策，单测覆盖先右后左、恢复余量、手动关闭、预览和子对话占宽。
- [x] 1.2 接入 App 与 ChatView 的实际宽度和显示意图，验证 resize 不写入持久化开关与栏宽。

## 2. Popup 交互

- [x] 2.1 实现左右互斥 popup、外部点击、按钮切换、Escape 与焦点恢复，浏览器验证栏内 portal 菜单不会误关闭。
- [x] 2.2 处理窄右侧详情与导航、页签草稿保留、极窄布局，浏览器验证实际 transcript 几何和刷新后的偏好恢复。

## 3. 集成验证

- [x] 3.1 执行完整 pnpm test、pnpm build、严格 OpenSpec 校验和 diff 检查，记录实际结果。
- [x] 3.2 Chromium 覆盖 1920×1080、较窄及极窄窗口、深浅主题和开关组合，保存截图并人工检查；注明 Desktop 包是否重建。

## 4. 原生窗口下限补齐

- [x] 4.1 将 Desktop 最小宽度降到 480 逻辑像素，使正常拖动能进入单列 transcript；核对 Windows/macOS 限制入口，验证原生下限与前端折叠阈值的跨层约束。

## 5. 浮层调宽

- [x] 5.1 增加左右浮层拖动与键盘手柄，按视口约束宽度并独立保存；单测验证范围、偏好兼容及不改变停靠判定。
- [x] 5.2 真实 App 验证详情、仅导航、左栏浮层的跨边界拖动、重新打开/刷新、视口夹取、取消及草稿保留；完成 Web 测试、构建与严格校验。

## 6. 降低折叠阈值

- [x] 6.1 将所有字号的 transcript 折叠阈值统一降至 350 CSS px，保留 64 px 恢复缓冲；更新边界与真实页面验证，完成 Web 测试、构建及严格校验。

## 验证记录

- 最终 `pnpm test`、`pnpm build` 均退出 0；构建扫描 4541 个正则，无旧 WebView 不兼容的 lookbehind。`openspec validate adapt-sidebars-to-transcript-width --strict` 与 `git diff --check` 通过。
- `node src/lib/responsiveSidebars.test.js`：通过。覆盖逐像素缩小、自定义左栏宽度、恢复余量、四种开关组合及预览/子对话占宽。
- `node scripts/test-responsive-sidebars.mjs`：通过。使用真实 App、TopBar、Sidebar、ChatView 和存储模块，仅隔离 API 与 WebSocket；覆盖外部点击、按钮切换、Escape、portal 菜单、列表自身收起、四组合刷新恢复、编辑器实例与未保存草稿、显式最大化、子对话占宽、切工作台清理 popup。
- Chromium 截图：1920×1080、1400×1080、1100×1080、800×1080、320×1080；深浅主题，已目视检查。产物目录由脚本输出，文件名包含 `1920-wide.png`、`1400-preview-popup.png`、`light-1920.png`、`dark-800.png` 等。
- Impeccable 对本次修改的 UI 文件检测返回空问题列表。
- 未重建或重启已安装 Desktop。Chromium 中通过 Desktop 编辑模式验证共用前端，不代表运行中的 Desktop 已加载更新。
- 原生下限补验：旧值 1170 在新增跨层用例中无法触发左栏折叠；改为 480 后，左栏宽度 160/270/360 的组合均能进入单列。`responsiveSidebars.test.js`、`singleLayout.test.js`、`framelessResize.test.js` 均通过。Windows 按 DPI 使用共享下限；macOS 拖动和程序化尺寸也使用该值。本轮未编译/启动原生 Desktop，需重建并重新启动后才改变运行中窗口的限制。
- 详情最大化拼接修正：去掉相邻文件栏圆角规则对最大化状态的排除。真实 App 的深浅主题验证均为圆角 0、相邻顶边与分隔位置一致，恢复普通布局后仍保持直角拼接。完整 `pnpm test`、`pnpm build` 通过；目视截图确认缺角消失，未重建 Desktop。
- 浮层调宽补验：纯逻辑覆盖浮层范围、视口临时夹取、旧偏好兼容与停靠判断隔离。`test-responsive-sidebars.mjs --popup-resize-only` 和完整脚本均通过，覆盖左右真实指针拖动、方向键、左栏缩到 160 px 仍保持浮动、从被夹取的实际边缘起拖、仅导航内容跟随宽度、详情编辑器实例与草稿、Escape/pointercancel 清理、关闭重开和刷新记忆。完整 `pnpm test` 与 `pnpm build` 退出 0；样式检测无新问题；已检查 `left-popup-resized.png` 与 `detail-popup-resized.png`。未重建已安装 Desktop。
- 350 px 阈值补验（2026-10-18）：纯逻辑验证各字号统一边界；真实 App 在默认栏宽下确认窗口 906/905 px 时右栏保持/折叠、626/625 px 时左栏保持/折叠，保持时 transcript 实测为 350 px；恢复分别在 970/690 px，保留 64 px 缓冲。完整浏览器脚本通过，包含浮层调宽、刷新记忆与草稿保留；`pnpm test`、`pnpm build`、严格 OpenSpec 校验与 diff 检查通过。截图输出目录为 `ace-responsive-sidebars-KoXviV`（系统临时目录）。未重建 Desktop。
