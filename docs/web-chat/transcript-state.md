# 实时 Transcript 状态所有权方案

Status: Approved for implementation

## 目的

规定 Web/Desktop 聊天中实时 transcript 状态的唯一所有者与唯一写入方式，
消除“渲染快照回写实时状态”造成的流式内容丢失。

## 现场结论

用户反馈长会话在高速出字时正文短暂“乱码”：已经显示出来的段落中间随机缺
汉字、标点和换行，而更靠后的文字已经出现；回合结束后内容又自动恢复正确。

对该会话的一次回复取证：

| 环节 | 结论 |
| --- | --- |
| 模型 / SSE | 正确 |
| `AgentLoop` 累计 | 正确，最终正文完整 |
| `EventDispatcher` / WebSocket | 244 个文本增量事件，序号连续无缺口 |
| 增量顺序拼接 | 与最终持久化的 355 字正文完全一致 |
| 连接状态 | 无断线、无重连、无乱序告警 |
| 浏览器状态合并 | **偶发丢片段** |

因此问题不在模型、服务端或传输，而在浏览器端的状态合并。

Markdown 不是根因，只是放大器：丢掉一个反引号、星号或换行，后面一整块的
结构和样式都会变形，看起来比实际丢失的几个 token 严重得多。

回合结束后恢复正常，是最终完整 `message` 与 self-heal 用权威快照覆盖了损坏
的流式草稿，属于兜底生效，不是问题消失。

## 失效路径

修复前，`useSessionTranscript` 里同一份实时状态有两个写入者：

1. `applyEvent` 在 WebSocket 事件到达时同步读写 `stateRef.current`；
2. 一个被动 `useEffect` 把某次渲染保存的旧 `state` 写回 `stateRef.current`。

React 18 的被动 effect 在提交后异步刷新，而 WebSocket 消息、REST 回调与定时
刷新都是独立的宏任务，两者的相对顺序由事件循环决定：

```text
提交 A+B                    -> 排入被动 effect 任务 P
WS token C 到达             -> 读 stateRef(A+B),写 stateRef(A+B+C)
P 执行(闭包里是 A+B)       -> stateRef 被写回 A+B          <- 丢失点
WS token D 到达             -> 读 stateRef(A+B),写 stateRef(A+B+D)
```

C 已被正确接收并推进了序号，所以这条路径在观测面上完全隐形：

- 传输层看不出乱序，序号去重不会拦截任何事件；
- `lastSeq` 依然单调增长，只是跳过了 C；
- 服务端 replay 数据完整；
- 只有页面正文随机缺少中间片段。

长会话渲染更慢，React 更容易让出主线程，提交与被动 effect 刷新之间的窗口
更宽，因此同样的代码只在大会话上稳定复现。

## 设计原则

1. 实时状态只有一个所有者，只有一个写入口。
2. 每次状态推进都必须基于最新已提交状态计算。
3. 渲染层只订阅状态，不得持有任何反向写入口。
4. “读取当前状态 → 计算 → 写回”必须在一次提交内原子完成。
5. 最终完整消息与 self-heal 保持为最后一道保险，而不是修复手段。

## 总体结构

```text
WebSocket 事件 / 历史加载 / 追赶重放 / 定时刷新 / UI 动作
                        |
                        v
        transcriptStore.commit(producer)      <- 唯一写入口
                        |
                        v
              已提交状态(单一真相)
                        |
                        v
        useSyncExternalStore 订阅(只读)
                        |
                        v
     transcriptProjection -> transcriptWindow -> ChatView DOM
```

## 写入契约

`web/src/lib/transcriptStore.js`

| 成员 | 契约 |
| --- | --- |
| `getState()` | 返回当前已提交状态；两次提交之间对象身份稳定 |
| `commit(producer)` | 唯一写入口；`producer` 必须是函数，入参恒为最新已提交状态 |
| `subscribe(listener)` | 通知不携带状态，订阅者必须重新 `getState()` |
| `getRevision()` | 状态版本号，单调递增，供诊断与测试断言 |

`commit` 只接受 producer 是刻意的：值形式的写入天然允许调用方传入一份过期
快照，而 producer 形式在入口就消灭了这种可能。

返回同一对象或空值表示“无变化”，不升版本、不通知订阅者，因此 reducer 对
重复/过期事件的幂等结果不会引发多余渲染。

订阅者在通知里再次提交（例如 self-heal 覆写）时，嵌套 producer 仍读到最新
状态；通知循环用重入标记保证既不丢通知，也不会递归重复广播同一份状态。

## React 边界

`useSessionTranscript` 用 `useRef` 持有 per-hook store 实例，渲染值来自：

```js
const state = useSyncExternalStore(subscribeStore, getStoreSnapshot, getStoreSnapshot);
```

`useSyncExternalStore` 是 React 18 面向外部可变数据源的官方入口，保证渲染读到
的快照与提交一致，并在快照于渲染中途变化时重新渲染。hook 内不再存在
`useState` 镜像，也不存在任何 `setState`，所以“渲染快照”与“实时状态”不可能
再分裂成两份真相。

代价是快照在渲染中途变化时会多一次重渲染。这是用可见的少量重渲染换取正确性；
长会话的 DOM 量已由尾部窗口限制，单次渲染成本可控。

## 单次原子提交

以下写路径全部收敛为单次 `commit(producer)`，读取与写回之间不再有时间差：

| 写路径 | 提交内容 |
| --- | --- |
| WebSocket 事件 | `reduceTranscriptEvent`，附带 effects 经闭包带出后再派发 |
| 会话切换重置 | 属于新会话的初始状态 |
| 历史加载 | 加载结果与实时尾巴的防回退合并 |
| 追赶重放 | `applyTranscriptReplayEvents` |
| 定时刷新 | 重新加载的历史 |
| 加载失败 | `loadState: 'error'` |
| 标题更新 | 新标题 |
| self-heal 覆写 | 权威历史加载、最近一轮比对与覆写 |

对外仍导出 `getState` 与 `updateState`，但 `updateState` 只接受 producer。
`ChatView` 的 self-heal 覆写把“读取当前状态、加载权威历史、比对最近一轮”整体
移入 producer，因此不再存在“读到的状态”与“写回的状态”之间的窗口。

## 交互契约

- 流式过程中任意时刻已呈现的 assistant 正文，必须是该回合最终正文的前缀。
- 序号不大于已应用序号的事件必须被忽略，且不改变流式正文。
- 历史加载与流式并发时，更完整的实时尾巴优先，界面不得被更旧的快照截断。
- 会话切换后，上一会话的内容不得残留。
- 窗口边界、tail-follow 与手动回看语义不变；本方案不新增任何 `scrollTop` 写入。

## 未覆盖范围

- 不新增流式分块协议字段（`turn_id` / `chunk_index` / `content_offset`）。现场
  证据显示服务端序列连续且完整，本次先消除客户端双写；协议级完整性校验作为
  后续独立变更评估。
- 不改变渲染节流策略，不引入 animation frame 合批，避免把正确性修复与性能
  调整混在一起。
- 不改 daemon 事件生成、`seq` 语义、replay ring、Markdown 渲染与投影折叠。

## 验证契约

| 门槛 | 内容 |
| --- | --- |
| 流式前缀不变量 | 高速重放整段回复，每次提交后正文都是最终正文的前缀，收尾一致 |
| 回归见证 | 按同一时序驱动旧的双写模型，证明它确实产出非前缀正文 |
| 写入口守护 | 值形式的回写被 `commit` 拒绝 |
| 幂等 | 重复与低序号事件不改变正文，也不升版本 |
| 并发 | 更旧的历史快照不截断实时尾巴 |
| 会话切换 | 重置后不残留上一会话内容 |
| 架构守护 | hook 不出现渲染快照回写、`useState` 承载 transcript、值形式 `updateState`；`ChatView` 自愈在单次提交内读写 |

对应测试：`web/src/lib/transcriptStore.test.js`、
`web/src/lib/transcriptStreamIntegrity.test.js`、
`web/src/lib/transcriptStateOwnershipArchitecture.test.js`。
