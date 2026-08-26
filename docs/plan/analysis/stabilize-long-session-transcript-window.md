# 大会话 transcript window 稳定性分析

## 问题与目标

大会话通过 `transcriptWindow.js` 将初始 DOM 限制在最近约 120 个投影行。
`transcript_replace` 会重建所有 raw items 的临时 id，现有窗口仍保存旧 id，
因此切片函数 fail-open 到全量历史。目标是在替换、重试和后续流式输出期间
保持有界 DOM，同时不改变用户的 tail-follow 与历史回看意图。

规范来源：

- `docs/web-chat/README.md`
- `docs/web-chat/transcript-window.md`
- `docs/daemon-api.md` 中 retry `transcript_replace` 协议
- `openspec/changes/stabilize-long-session-transcript-window/`

## 模块拆分

| 模块 | 输入 | 输出 | 依赖 | 交付内容 |
|---|---|---|---|---|
| `transcriptWindow.js` | 投影行、旧 `anchorKey` | 稳定 key、协调后的 key、窗口切片 | item 的 `messageId` / `id` | 稳定身份与纯协调函数 |
| `ChatView.jsx` | `renderedItems`、per-session 窗口状态 | 当前 render 的有界 `windowedItems` | transcript window 纯函数 | 同步协调接线 |
| `transcriptWindow.test.js` | 合成与 reducer 重建数据 | 行为断言 | reducer、projection、window helper | 单元与跨模块回归 |

## 集成枚举

```text
sessionTranscript.reduceTranscriptEvent(transcript_replace)
  -> historyItemsFromMessages allocates new item ids
  -> projectCollapsedTranscriptItems rebuilds projected rows
  -> ChatView reconciles previous stable anchor key
  -> windowTranscriptItems returns bounded visible rows
  -> existing tail-follow decides whether viewport moves
```

必须验证的真实连接：

1. `sessionTranscript.js` 生成的新 items 仍携带持久化 `messageId`；
2. `transcriptProjection.js` 对 user 行保持该身份；
3. `ChatView.jsx` 在调用 `windowTranscriptItems` 之前使用协调结果；
4. `anchorKey = null` 的显式全量状态不被协调函数改写；
5. `chatScrollFollow.js` 无需修改，证明本任务没有新增自动滚动入口。

## 任务边界

本问题只有一个最小原子交付：稳定身份、组件接线和回归测试相互依赖，拆分会
产生无法独立验证的中间状态。因此使用单任务 `chat-scroll-001`，开发完成后由
独立 verify 代理审查。

## 风险

| 风险 | 处理 |
|---|---|
| `messageId` 缺失 | 使用命名空间化临时 item key；失效时同步重选 |
| 合法替换删除原边界 | 重新选择最新尾部窗口，不永久全量化 |
| 用户已选择显示全部 | `null` 为显式状态，保持不变 |
| render-phase 状态调整产生循环 | 只有协调结果与当前状态不同时更新，并用当前结果切片 |
| 修复引入新的滚动抢占 | 不改 `chatScrollFollow.js`，审查 `scrollTop` 写入差异 |
