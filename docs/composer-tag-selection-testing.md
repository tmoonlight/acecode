# Windows 聊天输入框 tag 探索性测试

测试日期：2026-09-19。基线：`b973acec`。工作分支：`codex/fix-composer-tag-selection`。

## 已复现及修复

| 操作 | 原行为 | 修复后行为 |
| --- | --- | --- |
| 单击附件或图片 tag | 附件不能选中，图片直接打开预览 | 单击选择整枚 tag；图片双击预览 |
| 单击 tag 后复制、剪切、打字、粘贴 | 看起来已选中，但部分操作丢失引用或没有效果 | 统一使用 tag 两侧的可编辑选区，操作准确作用于该 tag |
| 从 tag 内部向前或向后拖选 | 选区被困在不可编辑标签内部 | 拖选可跨文字、多个 tag 和换行；起止 tag 整体纳入 |
| 方向键移到 tag 后打字或粘贴 | Chromium 在不可编辑节点内不产生有效输入，字符被吞 | 键盘选中的 tag 与鼠标使用相同的编辑边界 |
| 连续无空格 tag，选中前一个后 Delete | 可能错误删除后一个 tag | 精确删除当前选中项，撤销恢复原顺序 |
| 输入 `/init ` 自动转换后 Ctrl+Z | 撤销记录保留旧节点路径，内容没有撤回 | 命令转换与触发编辑共享历史，支持撤销、重做 |
| `/` 或 `@` 候选打开时使用 Shift/Ctrl 等组合键 | 候选捕获选择、行首行尾导航或换行快捷键 | 组合键回到编辑器处理，形成选区时关闭候选 |
| 附件多选但纯文本长度为零 | 可能继续显示候选菜单 | 使用实际选区是否折叠的信息判断 |

## 回归覆盖

最终浏览器回归共 **94 项通过**：tag 编辑 78 项、候选菜单 16 项；没有浏览器运行错误。`pnpm test`、`pnpm build`、OpenSpec 严格验证及 `git diff --check` 均通过。明暗主题、中文/英文、390px 及减少动态效果共四种截图状态已检查。

`web/scripts/test-composer-selection.mjs` 直接加载生产 RichComposer 和样式，使用真实 Windows Chromium 鼠标、键盘和剪贴板操作。覆盖命令、技能、路径、会话、文件附件、图片附件，单选、双向拖选、tag 到 tag、文字到 tag、Shift 左右/上下/Home/End、全选、复制、剪切、粘贴、替换、Backspace/Delete、撤销/重做、重复附件、多行与 390px 换行。

`web/scripts/test-composer-completion.mjs` 加载生产 InputBar 和候选菜单，以固定文件及命令 API 验证候选弹出状态下的组合键、关闭选区候选、普通 Enter/Tab 和目录导航。当前 16 项全部通过；换回原父层源码后，其中 12 项失败，普通 Enter/Tab 的 4 项通过。

纯 Slate 测试覆盖命令历史、附件零长度边界、连续 tag、鼠标回调和多种插入操作。已有 IME 保护测试继续运行。Windows 全量测试中发现 SVG 文本断言依赖 LF，测试现在只对换行格式归一化，未改变 SVG 资源。

## 执行方式

在 `web` 目录执行：

```powershell
pnpm test
pnpm build
```

浏览器测试使用单独安装的 Playwright，不修改项目依赖：

```powershell
npm install --prefix "$env:TEMP/ace-composer-browser" playwright
$env:ACE_PLAYWRIGHT_MODULE = "$env:TEMP/ace-composer-browser/node_modules/playwright/index.mjs"
node "$env:TEMP/ace-composer-browser/node_modules/playwright/cli.js" install chromium
node scripts/test-composer-selection.mjs
node scripts/test-composer-completion.mjs
```

可用 `ACE_CHROMIUM_EXECUTABLE` 指定已安装的 Chromium。选择测试可用 `ACE_COMPOSER_TEST_OUTPUT` 保存 JSON 结果，附加 `--screenshots` 保存明暗主题及中英文窄屏截图；`ACE_COMPOSER_TEST_FILTER` 按用例名过滤。脚本启动独立本地 Vite 测试服务并在结束时关闭浏览器和服务，不连接运行中的客户端或真实账户。

## 验证边界

- 验证使用 Windows 本机 Chromium，真实生产 React 组件及键鼠操作，后端 API 使用固定 fixture。
- 未重新构建或安装 ACECode Desktop，未验证当前安装包里的 WebView2。
- IME 回归使用合成 composition/keyCode 229 事件和模型测试，未操作微软拼音真实候选窗口。
- impeccable 检测发现的 12 处提醒均来自 globals.css 中未修改的既有样式，本次变更行未命中。
- 测试和修复在独立 worktree 完成；未发布版本或操作用户正在使用的 Codex 草稿。

## 2026-09-20：文件拖放后的系统键盘焦点

基线 `9dfd0e11`。Windows 拖放进入时的窗口激活失败后，WebView2 仍可能显示正常光标并报告 `document.hasFocus() === true`，但系统前台仍是来源窗口，直接按键不会进入 composer；再次点击输入框才恢复。

- 接收有效文件时，前端先调用 `aceDesktop_focusFileDropWindow` 再解析文件。Windows 临时连接前台线程的输入队列，恢复窗口后立即断开，再将键盘焦点交给 WebView2。无通知置顶兜底、定时重试或异步完成后的抢焦点。
- `web/scripts/test-composer-file-drop.mjs` 的 7 项生产 InputBar 回归通过：Windows/macOS 原生回调、Linux URI、浏览器上传、批量文件、直接输入及方向键、异步切换窗口、重复/空回调和中途禁用。该脚本使用固定桥接，并不声称在 macOS/Linux 原生系统上运行。
- 同组测试替换为修复前 InputBar 时 6 项失败。新增桥接优先级和旧版壳回退测试与全量 `pnpm test` 通过；`pnpm build` 和正则兼容检查通过。
- Windows 原生验证使用当前 `WebHost` 代码编译的隔离窗口与生产 InputBar。以真实 OLE 文件拖放和系统键盘输入验证：拒绝第一次进入激活时，基线保留 tag/光标却收不到按键；修复后无需点击即可提交系统中文输入法候选，再输入英文并使用左方向键插入文字。
- 原生验证未触碰用户正在使用的 Codex 草稿。详细过程日志位于临时目录 `acecode-drop-baseline-final.log`、`acecode-drop-native-final.log`。
- 当前 Windows Release 客户端与 daemon 已增量构建并重新运行；实际客户端检查了启动与所服务开发前端的构建一致性。

复跑浏览器回归：沿用上述 Playwright 环境设置后，在 `web` 中执行 `node scripts/test-composer-file-drop.mjs`。

## 2026-09-20：统一拖入与粘贴文件

- `composerFileIntake` 负责路径、原生剪贴板和浏览器 File 的统一分类；原生文件引用不读取字节，桌面无源路径数据保存在 `composer-files/<uuid>/` 后引用，纯 Web 保留上传。
- RichComposer 的文件事务持有真实 Slate rangeRef，保留混合选区中的零文本长度附件。批量文件只产生一次撤销；等待期间继续编辑、移动光标、切换任务/工作目录或清空草稿均受保护。多次原生请求即使倒序完成，也按手势顺序插入。
- 扩展后的文件输入脚本 19 项通过，比较拖入与粘贴的结构化内容、文件名、完整路径、样式、混合选区替换与撤销；同时覆盖文本格式互斥、本地保存、纯 Web 上传、错误和异步焦点。`ACE_COMPOSER_DROP_TEST_FILTER` 可过滤用例，`ACE_COMPOSER_TRANSFER_SHOT_DIR` 可保存配对截图。
- 既有选择回归 80 项通过；`pnpm test`、`pnpm build`、OpenSpec 严格校验及差异检查通过。截图中的拖入与粘贴 tag 外观一致，保留原有圆角与独立矩形选区。
- Windows 原生文件处理 7 项测试通过，包括中文名称、同名文件独立保存、二进制字节、失败批次回滚与原始大文件引用。macOS/Linux 的桥接由当前平台适配代码实现，路径与入口契约由 Windows 浏览器 fixture 检查，未在这两个系统上实机验证。
- 本次 Windows Release 原生支持库与 Desktop 已重新编译、链接并启动，实际客户端返回的前端资源与本次构建哈希一致；交互验证使用生产组件 fixture，不操作真实任务草稿。

## 2026-10-01：图片恢复缩略图

- 上一节的「原生文件引用不读取字节、无源路径数据落 `composer-files/` 再引用」对栅格图片撤回：Desktop 粘贴截图、拖入、选择或从资源管理器复制的 PNG/JPEG/GIF/WebP/BMP 等图片重新作为快照附件（输入框与对话记录显示缩略图，模型直接收到图片）。原因：服务端拒收图片引用（`image attachments require snapshot data`），路径引用的截图只显示文件名，模型还要先 `bash` 再 `show_image` 才看得到。
- `materialize_context_items` 对 25 MiB 以内的栅格图带回字节；超限或读取失败退回路径引用，SVG、普通文件与文件夹不变。`composerFileIntake` 把带字节的图片条目还原成带来源路径标记的 File 走上传，同一手势里的其它条目照旧插 `@路径`。
- 对话记录中用户消息的图片附件改由气泡上方的缩略图条渲染，不再作为正文里的文件名按钮；只有图片的消息不画空气泡。
- 验证：`pnpm test`、`pnpm build` 通过；`DesktopContextItems.*` 8 项通过；在运行中的 Desktop 页面上确认旧会话的图片消息显示缩略图，并以桩桥接确认截图粘贴与「图片 + 普通文件」混合粘贴的输入框表现。`test-composer-file-drop.mjs` 需要 Playwright，本次未运行。

## 2026-10-04：tag 排版与普通方向键

此处覆盖前文历史行为：tag 为无背景的主题色文本，与正文使用相同字体、字号、行高和基线，仅稍加粗。选中时蓝底白字。普通左右键不再进入 tag 选择状态，而是一次越过一个 tag；Shift 和鼠标仍可选中，选中后复制、剪切、替换、删除和撤销保持原有行为。

- 生产 RichComposer 浏览器回归 89 项、InputBar 候选回归 16 项全部通过，包括连续 tag、空格、段落边界、RTL 和混合选择；定向模型测试、构建、OpenSpec 严格检查与差异检查通过。
- 本轮全量 `pnpm test` 被当前工作树其它改动的三个未登记静态文案阻断，详见 `openspec/changes/fix-composer-tag-selection/validation.md`。未改动无关文案或翻译目录。
- 验证平台为 Windows Chromium 生产组件 fixture；未重建或安装 Desktop/WebView2，未在真实输入法候选窗口人工验证。
