# DeepSeek 轨迹同步记录

来源：`C:\Users\shao\deepseek-harness`，`deepseek-ai/deepseek-harness` 本机提交 `687f282e86`（2026-10-17）。本次比较的是用户指定的本机检出，不代表远端最新版本。上游文件采用 MIT 许可，完整声明见 `LICENSE.deepseek`。

## 已同步

| 上游变化 | ACECode 对应实现 |
| --- | --- |
| `31ff3a73c0`、`37c2fa40ac`、`9bdd420512`：流式/中断/重试状态 | `trajectoryModel.js` 区分模型响应与工具步骤结束；重试中的请求保持等待，恢复后保留上次错误；中断输出可跳转检查。详情使用新状态判定。 |
| `e779831f40`：已记录助手计时 | 流式保留已记录开始/首输出时间，完成时间使用响应记录，不把后续工具时间算入模型耗时。没有记录的指标保持不可用。 |
| `18739de7c6`、`3a32c617a3`：JSON 字符串 | 新 `JsonTree` 支持长字符串展开、限高、换行和复制；默认偏好采用 `ace.trajectory.jsonStringWrapping`，其他已展开项不受影响。 |
| `632b8e19b4`、`b6858789f0`：代码详情 | 按记录中的 `run_code` 名称识别，语言来自记录时 schema，支持行号、代码/原始 JSON 切换、精确复制。没有添加代码执行工具。 |
| `f36e1a3d1d`、`f0810edd7f`、`e5038fcf9e`、`039415d5a4`：详情阅读 | 轮次/步骤标题、默认展开思考、紧凑 Markdown，保留 ACECode 的数学和中文 Markdown 修复。 |
| `c27de594fd`、`1da97f30f8`：附件 | 混合附件顺序和重复引用、文件名截断、零字节大小、原始字段折叠；图片复用 ACECode URL 和 `ImageLightbox`。 |
| `2ab37e9558`、`2e21d210a5`：驻留历史窗口 | 首次最近 50 节点；先展开已加载的历史再请求前页；流式追加不逐出窗口前缀，替换历史后重设锚点，工具跳转自动展开所需驻留记录。请求编号及累计用量仍使用全部已加载记录。 |
| 上游详情和 JSON 组件后续优化 | 同步有实际功能差异的组件逻辑，保留 ACECode 图标、主题和兼容回退。 |

## 已有能力与适配边界

- `trajectory-virtual-rows.ts` 与此版本上游一致；时间线投影差异只有本地化参数，缩放、区间筛选、悬停和滚动交互已具备。搜索索引补充附件名称。
- 保留 `inputTurns`、`turn_id/user_message_id` 归属、实时去重及 `inspectCallId` 跳转。轨迹继续沿用现有英文标签；上游语言字典只作为组件翻译接口的来源，未改变应用的语言设置。
- 保留 WebView 109 的颜色回退、共享图标和现有 Markdown；新增遮罩仅在特性支持时启用。JSON 深色主题使用 ACECode 的主题选择器。
- 不移植上游 Cordis/Session V4/Definition 注册、图片 Slot/授权缓存、PTC 执行引擎及专属 developer 工具更新事件。ACECode 没有这些事件协议，不通过猜测生成记录；现有请求快照中的提示词和工具目录差异继续显示。
- 现有前端仍会将已加载的原始记录投影为数据节点；本次避免全量重复生成布局并限制 React 可见窗口，没有宣称迁移上游全套增量 Session 引擎。

## 验证

从仓库根目录执行：`node web/src/lib/trajectoryModel.test.js`、`node web/src/lib/trajectoryInspection.test.js`、`node web/scripts/test-trajectory-turn-order.mjs`、`node web/scripts/test-trajectory-inspector.mjs`、`pnpm --dir web test`、`pnpm --dir web build`。

浏览器脚本可通过 `ACE_PLAYWRIGHT_MODULE` 指定 Playwright 模块路径，`ACE_CHROMIUM_EXECUTABLE` 指定已有 Chromium；截图写入系统临时目录。完整验收记录见 `openspec/changes/sync-deepseek-trajectory-updates/verification.md`。源码及 Web 构建验收不代表已安装 Desktop 已替换或重启。
