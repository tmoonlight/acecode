## 1. 实现

- [x] 1.1 接入按钮显示测量、浮层定位、通用阴影和返回底部动作，更新中英文文案。
- [x] 1.2 验证显隐边界与恢复跟随状态，完成浏览器交互及主题/窄屏检查。

## 2. 验收

- [x] 2.1 运行完整 Web 测试、构建、OpenSpec 严格校验与差异检查，记录结果。

## 3. 到底后箭头残留回归（2026-10-04）

- [x] 3.1 修正底部容差并同步程序滚动后的按钮显隐，补充边界测试。
- [x] 3.2 添加并运行真实 ChatView 浏览器回归，确认旧代码复现、新代码通过，覆盖缩放、手动滚动、点击返回、流式跟随和短会话。
- [x] 3.3 运行完整 Web 测试、构建、OpenSpec 严格校验与差异检查，记录本次验收结果。

## 4. 运行中的三点指示

- [x] 4.1 复用会话运行状态替换按钮图标，增加从左到右顺次跳动和减少动态效果样式。
- [x] 4.2 增加并执行真实 ChatView 状态切换、动画次序、点击、会话切换与窄屏主题回归。
- [x] 4.3 运行前端测试、构建、OpenSpec 严格校验与差异检查，记录结果及验证边界。

## 5. 三点在系统减少动态效果下静止

- [x] 5.1 核对 Windows 动态效果设置及当前 daemon 实际提供的 CSS，确认静止来自 reduce 分支关闭动画，而非旧包。
- [x] 5.2 减少动态效果时保留低幅顺次跳动，调整周期避免长时间全部静止；新增真实时间采样回归，证明旧样式失败、新样式通过。
- [x] 5.3 完成生产组件及构建样式验证、全量前端测试、构建与文档核对，保留先前图片粘贴及弹层关闭修复。

## 验证记录（2026-10-05：三点真实时间静止回归）

- Windows `SPI_GETCLIENTAREAANIMATION` 实测 false，Chromium 系统默认 `prefers-reduced-motion: reduce`。旧样式在真实时间 2.6 秒、84 帧内三点位移均为 0，animationName 为 none，新增回归先失败。
- 新样式不更改系统设置；reduce 下保留 1.5px 轻微顺序跳动，普通模式 3px；延迟改为 0/400/800ms，周期 1.2 秒，各点依次跳动，不再有半个周期全静止的间歇。
- 生产 ChatView 浏览器回归 27 项通过，覆盖系统默认、no-preference、reduce 三种情况下至少两轮真实时间动画。关键帧采样单独检查左右顺序；不再只靠暂停/跳转关键帧验证。
- 当前 Desktop daemon 的 HTTP 页面与本次 web/dist 构建 SHA-256 相同；解析实际响应中的 style 后独立运行，系统默认下实测三点分别移动约 1.497/1.497/1.500px，普通模式约 3px，所有动画均 running。此检查没有修改用户页面或系统配置。
- 先前图片粘贴与图标选择层回归重新执行 13/13 通过；完整前端测试退出码 0，构建成功（3133 模块、4500 个正则兼容检查）。OpenSpec strict 与本任务差异检查通过。
- 结果位于会话临时目录 `dot-motion-before/`、`dot-motion-after/`、`context-paste-recheck/`；服务端产物验证脚本为同目录 `verify-served-dots.mjs`。当前客户端使用本工作树的 web/dist，F5 刷新后加载新资源；未强制刷新用户正在运行的页面，也未发布新版安装包。

## 验证记录（2026-10-04：运行中的三点，历史稿）

- 复用既有会话运行状态，只替换圆形按钮内的 18px 图案；三个圆点依次上跳，每个相隔 200ms，周期 1.2s，不改变点击、显隐及历史阅读逻辑。减少动态效果时显示静态三点。
- 真实 ChatView 的 Windows Chromium 浏览器检查 24 项通过，无运行时异常。覆盖推理触发、逐点动画时间采样、按钮几何不变、完成/错误/中断恢复箭头、流式输出、鼠标和键盘返回、减少动态效果、缩放取整、窄屏暗色、REST busy 快照恢复以及会话切换。
- 滚动、架构及活动展开锚定定向测试 28 项通过；`corepack pnpm test` 全量退出码 0；`corepack pnpm build` 通过（3132 模块，4500 个正则字面量兼容性检查通过）。
- OpenSpec 严格校验及本任务相关差异检查通过。截图与浏览器结果位于当前会话临时目录的 `chat-tail-progress/`。
- 测试只使用隔离 API fixture 和生产前端组件，未启动、重新打包或替换已安装 Desktop，未操作用户真实会话。

## 验证记录（2026-10-04：到底后箭头残留）

- 修复：独立的 2px 按钮显隐容差兼容滚动指标取整；每次受保护的程序滚动后立即更新显隐，不改变 80px 自动跟随阈值与历史阅读状态机。
- `node src/lib/chatScrollFollow.test.js`、`chatScrollFollowArchitecture.test.js`、`activityExpansionAnchor.test.js`：26 项通过。
- 新增 `web/scripts/test-chat-scroll-to-bottom.mjs`，可使用 `ACE_PLAYWRIGHT_MODULE` 与 `ACE_CHROMIUM_EXECUTABLE` 指定现有 Playwright/Edge，无需 daemon 或真实会话。
- 浏览器旧代码对照：`node scripts/test-chat-scroll-to-bottom.mjs --baseline` 在“点击到达缩放后的最大滚动位置却仍显示按钮”处按预期失败，当时剩余距离约 1.148px；直接布局探测也复现过约 1.407px 的误差。
- 浏览器新代码：同一脚本 12 项通过，无运行时错误。覆盖初始化、缩放取整、点击、真实鼠标滚轮、24px 显示、历史阅读、键盘返回、流式跟随、无 scroll 事件的程序滚动、窄屏暗色和短会话。窄屏用例在响应式布局稳定后重新建立历史阅读前置状态。
- `pnpm test`：2983 个 `[pass]`，退出码 0。
- `pnpm build`：通过，4498 个正则字面量兼容性扫描通过。
- `openspec validate add-chat-scroll-to-bottom --strict`、`git diff --check`：通过；Impeccable detector 返回空问题列表。
- 浏览器截图、测试和构建日志位于系统临时目录 `acecode-tail-visibility-20261004`。本次仅修改并验证当前检出的 Web 源码，未重新打包或替换已安装 Desktop。

## 验证记录（2026-10-01）

- `node src/lib/chatScrollFollow.test.js`、`chatScrollFollowArchitecture.test.js`、`activityExpansionAnchor.test.js`：24 项通过。
- `pnpm test`：完整 Web 测试通过，日志含 2907 个 `[pass]` 结果。
- `pnpm build`：通过，构建后的正则兼容性扫描通过。
- `openspec validate add-chat-scroll-to-bottom --strict` 与本次涉及文件的 `git diff --check`：通过。
- Impeccable detector：本次 ChatView 扫描无问题。
- Edge 浏览器：加载真实 ChatView 及其样式，模拟 REST/流式事件，20 项交互检查通过，无运行时错误。覆盖底部隐藏、历史显现、24px 近底位置、流式阅读不抢滚动、点击后跟随、Enter/空格、亮暗主题、390px 窄窗口、英文名称、待办浮层避让、长短会话切换、视口尺寸变化和新会话首页。
- 浏览器使用隔离的模拟会话；未重新打包或启动安装版 Desktop。临时验收文件位于系统临时目录 `acecode-jump-tail`。
