# 验证记录

日期：2026-10-11。当前 master 工作区内实施；保留已有其他改动。

## 根因与现场只读复验

- 原始真实事件顺序为初始化上下文、用户消息、turn_start；前端分别创建了三个回合。
- 修复前四次输入投影为 `[3, 5, 7, 9]`；修复后，同一真实接口数据投影为 `[1, 2, 3, 4]`。
- 另一条单次输入的测试会话修复后投影为 `[1]`。
- 现场接口仅执行 GET，没有修改会话或重启运行中的 Desktop/daemon。

## 自动化验证

- `node web/src/lib/trajectoryModel.test.js`：通过。覆盖上下文、两种生命周期顺序、不同身份别名、重复开始/结束边界、连续四轮每条事件前缀、无助手结果、身份优先于过时编号、tail 缺少开始事件、引导与工具注入输入、完成后 partial 去重。
- `node web/src/lib/sessionTranscript.test.js`：通过。覆盖活动回合身份进入 partial，以及历史快照缺少开始事件时恢复身份。
- `node web/scripts/test-trajectory-turn-order.mjs`：Chromium 无头浏览器通过，无页面异常。通过 `ACE_PLAYWRIGHT_MODULE` 与 `ACE_CHROMIUM_EXECUTABLE` 复用本机已安装工具。
- 浏览器回放覆盖初始化上下文的首轮、六轮历史后第七轮先流式输出、用户消息/turn_start/model_request/model_response 分段轮询及最终结果去重。聊天状态的计数故意保持过时，轨迹仍按活动身份定位。
- 浏览器截图：`C:/Users/shao/AppData/Local/Temp/ace-trajectory-turn-order/trajectory-turn-order.png`。
- `pnpm test`：最终代码通过；日志包含 3110 条 `[pass]`，Node TAP 汇总 3/3，通过且无失败。
- `pnpm build`：最终代码通过；构建后的 4524 个正则通过旧 WebView lookbehind 兼容检查。
- `openspec validate fix-trajectory-turn-order --strict`：通过。
- 本次六个已跟踪修改文件的 `git diff --check`：通过；新增浏览器脚本独立保留。

## 验证边界

上述为当前源码、真实只读响应投影及浏览器组件回放结果。已安装 Desktop 尚未重打包或替换，不能据此宣称当前运行的安装版已经包含修复。OpenSpec change 目录遵循当前仓库忽略规则，文档保留在本地。
