# Web/Desktop 聊天设计范围

## 目的与边界

本范围定义共享 Web UI 中聊天 transcript 的展示契约。Desktop 通过 WebView
复用同一实现，因此 `web/src/components/ChatView.jsx` 是 Web 与 Desktop 的共同
视口所有者。

本范围负责：

- 把 session transcript state 投影为可见行；
- 对大会话执行尾部窗口化，限制同步 DOM 工作量；
- 管理 tail-follow、历史回看、查找和回合跳转；
- 在 `transcript_replace`、历史加载和实时事件之间保持相同的展示语义。

本范围不负责 session 持久化、provider 重试、daemon 事件生成或 Markdown
解析规则。这些分别由 session、provider、daemon 和 Markdown 渲染模块拥有。

## 关系与所有权

```text
daemon WebSocket / history API
             |
             v
  sessionTranscript.js       owns reduced transcript state
             |
             v
 transcriptProjection.js     owns presentation-only grouping
             |
             v
  transcriptWindow.js        owns bounded visible slice
             |
             v
      ChatView.jsx            owns DOM, viewport and tail-follow
             |
             v
       Web / Desktop user
```

| 状态或动作 | 所有者 | 说明 |
|---|---|---|
| 实时状态所有权 | `singleWriterStore.js` | 单写者 store,`commit(producer)` 是唯一写入口;transcript 与排队输入共用 |
| 原始 transcript items | `sessionTranscript.js` | 历史加载与实时事件归并 |
| 折叠后的投影行 | `transcriptProjection.js` | 只影响展示，不删除原始数据 |
| 窗口边界 | `transcriptWindow.js` + `ChatView.jsx` | 纯函数计算，组件持有 per-session 状态 |
| tail-follow / review | `ChatView.jsx` + `chatScrollFollow.js` | 用户滚动意图优先于自动跟随 |
| `transcript_replace` 生成 | daemon / `AgentLoop` | UI 只消费替换事件，不决定重试 |

## 生命周期

1. `useSessionTranscript` 先加载历史，再接收实时 session 事件。
2. `projectCollapsedTranscriptItems` 生成展示投影。
3. `ChatView` 为大会话选择首个可见边界，默认仅挂载尾部窗口。
4. 流式追加保持窗口边界不变；用户可逐段揭示或显式显示全部。
5. 历史替换后窗口边界必须与新投影重新协调，不能永久退化为全量 DOM。
6. tail-follow 只在用户仍处于跟随状态时移动视口。

## 入口与约束

- 组件入口：`web/src/components/ChatView.jsx`
- 实时状态 store：`web/src/lib/singleWriterStore.js`
- Transcript reducer：`web/src/lib/sessionTranscript.js`
- 展示投影：`web/src/lib/transcriptProjection.js`
- 尾部窗口：`web/src/lib/transcriptWindow.js`
- Tail-follow：`web/src/lib/chatScrollFollow.js`

跨模块约束：

- 实时 transcript 状态只有一个写入者;渲染层只订阅,不得反向写回渲染快照。
- 流式过程中任意时刻已呈现的 assistant 正文,必须是该回合最终正文的前缀。
- 窗口边界必须使用跨 reducer 重建仍稳定的消息身份。
- 任意自动滚动都必须尊重既有 follow/review 状态。
- 窗口化不得改变查找、回合跳转、fork 或 raw transcript 消费者的数据范围。
- 找不到边界时必须保证内容不丢失，但主视图必须在同一 render 中恢复有界窗口，
  不能先绘制一次全量历史。

## 模块文档

- [transcript-state.md](transcript-state.md)：实时状态所有权、单写者提交契约和流式前缀不变量。
- [transcript-window.md](transcript-window.md)：大会话尾部窗口、替换协调和测试契约。
