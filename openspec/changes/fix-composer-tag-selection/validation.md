# 验证记录

2026-09-19，Windows，基线 b973acec。详细矩阵和复跑命令见 `docs/composer-tag-selection-testing.md`。

- RichComposer 浏览器键鼠回归：78/78 通过；浏览器异常与 console errors 均为零。
- InputBar 候选菜单浏览器回归：16/16 通过；旧版同组 12 项失败。
- 纯 Slate 命令同步 11 项、选区与指针 14 项，以及已有键盘/IME、草稿、生命周期测试通过。
- `pnpm test` 通过。SVG 测试对 Git 的 CRLF checkout 归一化后可直接在 Windows 重跑，无 SVG 资源变更。
- `pnpm build` 通过，3077 模块，4450 个正则字面量兼容性扫描通过。
- `openspec validate fix-composer-tag-selection --strict` 与 `git diff --check` 通过。
- impeccable detector 完成，12 条提醒均在 globals.css 未变更的既有代码；本次修改行无提醒。
- 明暗主题、中英文、390px 与 reduced-motion 四种截图已检查，选择填充完整，无横向溢出。

原始浏览器结果和截图保存在 `C:/Users/shao/.codex/visualizations/2026/09/19/01a0b9c0-da2f-7001-bc01-1fb359fafdaf/composer-selection/final/`。

测试使用生产组件和真实 Windows Chromium 鼠标/键盘事件。未重新构建已安装 Desktop/WebView2，未操作微软拼音实际候选窗或用户正在运行的 Codex 草稿；IME 为合成事件及模型回归。

## 2026-09-20：按确认稿增加矩形选区背景

基于 `47d20596`，在独立 worktree `composer-selection-visuals` / 分支 `codex/fix-composer-selection-visuals` 中实现。

- 原始圆角徽标与选区层分离，选中只在后方新增矩形，保留底色、边框、文件类型图标颜色和上传状态。未选中与选中时的徽标几何完全一致。
- 前置图标采用与字体对应的尺寸；Seti 字形只放大实际绘制区域，不扩大布局和点击范围。内边距左右相等，移除单边外间距与附件移除按钮的负间距。
- 原有 78 项 Windows Chromium 选择与编辑检查全部通过；新增长路径窄屏检查通过。新增视觉检查覆盖明暗主题、13/14/16/20px 字号、390px、上传状态、取消选区及失焦；首次发现图标选区颜色继承问题，修正后定向复测通过。合计 80 个用例已通过，无浏览器异常。
- `pnpm test` 与 `pnpm build` 通过；构建 3077 模块，4450 个正则字面量兼容性扫描通过。
- `openspec validate fix-composer-tag-selection --strict` 与 `git diff --check` 通过。
- impeccable detector：11 条提醒、1 条 advisory，均位于未修改的既有样式，本次改动行无提醒。

原始完整回归与视觉复测结果分别保存在前述可视化目录的 `additive-final/results.json` 和 `additive-final/visual-regression/results.json`；真实组件截图位于 `additive-confirmed/`。验证范围为生产组件的 Windows Chromium 渲染与交互，本轮没有重新构建或切换用户正在运行的 Desktop 客户端。

## 2026-09-20：拖入文件后继续输入

基线 `9dfd0e11`。修复范围为文件释放时的原生键盘焦点恢复，完整路径、文件名标签展示与选择样式保持原有行为。

- 新增生产 InputBar 浏览器回归 7 项通过；基线同组 6 项失败。覆盖平台入口、直接输入与方向键、重复/无效回调、禁用状态以及异步完成不抢前台。
- 使用当前生产 `WebHost` 编译 Windows 隔离窗口，以真实 OLE 文件拖放和系统键盘验证。基线在拒绝第一次拖入激活后仍显示光标，但按键留在来源窗口；新桥接恢复窗口和 WebView2 焦点后，中文输入法候选提交、英文输入、左方向键插入均通过，无需额外点击。
- `pnpm test`、`pnpm build`、`openspec validate fix-composer-tag-selection --strict` 与 `git diff --check` 通过。没有新增编辑器依赖或修改文件引用协议。
- Windows Release `acecode-desktop` 及其 daemon 依赖增量构建通过，客户端已通过仓库 Desktop 开发启动器重新运行。原生交互验证在隔离窗口完成，实际客户端验证启动与开发前端资源一致性。

## 2026-09-20：统一拖入与粘贴文件

- 基线 `8a7bf7c5`。桌面原文件、剪贴板文件和选择器共享文件分类与 Slate 插入事务；无源路径数据先保存本地，纯 Web 才上传。原生剪贴板错误不再降级为空结果，普通文本不被猜测为本地文件。
- 文件输入浏览器回归 19 项通过，覆盖同文件拖入/粘贴结构与样式相等、Windows/UNC/POSIX 路径、文件夹、图片、多文件、混合选区、零长度附件、一次撤销、连续输入、异步乱序、焦点、清空草稿及任务/工作目录切换。
- 原有 tag 选择回归 80 项通过；原生文件处理 GoogleTest 7 项通过，当前 Windows WebHost 编译通过；`pnpm test`、`pnpm build`、OpenSpec 严格验证、差异检查及本次组件的 impeccable 检查通过。
- 配对截图在临时目录 `acecode-file-intake-shots/drop.png`、`paste.png`，逐项样式与结构比较通过。macOS/Linux 尚未实机验证；浏览器中的平台分支使用桥接 fixture。
- Windows Release 原生支持库与 Desktop 编译、链接通过，使用当前工作树的 Desktop 启动器重新运行；HTTP 返回的开发前端 SHA-256 与 `web/dist/index.html` 一致。相邻 daemon 的 API 无变更，复用现有可运行产物。
- 功能提交 `9f1a847c`，合入远端 macOS 窗口按钮调整后，以 `b5aee5ba` 推送到 `origin/master`，远端提交哈希回读一致。

## 2026-10-04：普通左右键直接越过 tag

- 同一工作树的变更前定向浏览器检查 8 项失败，命令、技能、路径、会话、附件均会先形成选区；真实 Slate 模型测试同样复现。修改后直接定位相邻 tag 另一侧的可编辑 point，不经过 void 内部选区，不使用附件为零的纯文本长度。
- Windows Chromium 生产 RichComposer 回归 89/89 通过，包含双向移动后输入、连续无空格 tag、段落首尾、RTL、Shift 原子选中、鼠标拖选、剪贴板、撤销和排版。候选菜单生产 InputBar 回归 16/16 通过。浏览器异常及 console errors 为零。
- `richComposerKeyboard.test.js`、`composerSelection.test.js`、`composerDropdownKeyboard.test.js`、`slateComposerArchitecture.test.js` 定向通过。IME 的十种组合状态均新增左右键保护检查。
- `corepack pnpm build` 通过（3132 模块、4500 个正则兼容检查）；OpenSpec 严格校验与本任务差异检查通过。
- `corepack pnpm test` 已执行，但被当前工作树并行改动中的三个未登记文案阻断：`ChatView.jsx` 的“停止请求发送失败，连接恢复后请重试”、`goalControl.js` 的“正在等待当前任务停止”、`sessionTranscript.js` 的“正在停止本轮任务…”。失败来自 `sourceCatalog.test.js` 的目录覆盖断言，本任务未修改这些文件或绕过检查，不宣称全量通过。
- 测试原始结果在本会话临时目录的 `composer-arrows-baseline/results.json` 和 `composer-arrows-final/results.json`。未重新构建或安装 Desktop/WebView2，未触碰真实草稿；系统输入法候选窗未人工验证。
